#include <assert.h>
#include <errno.h>
#include <libusb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../openhmdi.h"

#include "esp570.h"
#include "uvc.h"

#ifdef __linux__
#include <asm/byteorder.h>
#else
/* FIXME: Assumption of little endianness, which is pretty valid on non-LINUX */
#define __le32 uint32_t
#define __le16 uint16_t
#define __u8 uint8_t
#define __cpu_to_le16
#define __le16_to_cpu
#define __cpu_to_le32
#define __le32_to_cpu
#endif

#ifdef _MSC_VER
#define PACKED_STRUCT(name) \
    __pragma(pack(push, 1)) struct name __pragma(pack(pop))
#elif defined(__GNUC__)
#define PACKED_STRUCT(name) struct __attribute__((packed)) name
#endif


#define SET_CUR 0x01
#define GET_CUR 0x81
#define TIMEOUT 1000

#define CONTROL_IFACE   0

#define VS_PROBE_CONTROL	1
#define VS_COMMIT_CONTROL	2

#define RIFT_SENSOR_CLOCK_FREQ	(40000000)
#define RIFT_SENSOR_WIDTH       1280
#define RIFT_SENSOR_HEIGHT      960
#define RIFT_SENSOR_FRAME_SIZE  (RIFT_SENSOR_WIDTH * RIFT_SENSOR_HEIGHT)

#define VERBOSE_DEBUG 0

PACKED_STRUCT(uvc_probe_commit_control) {
	__le16 bmHint;
	__u8 bFormatIndex;
	__u8 bFrameIndex;
	__le32 dwFrameInterval;
	__le16 wKeyFrameRate;
	__le16 wPFrameRate;
	__le16 wCompQuality;
	__le16 wCompWindowSize;
	__le16 wDelay;
	__le32 dwMaxVideoFrameSize;
	__le32 dwMaxPayloadTransferSize;
	__le32 dwClockFrequency;
	__u8 bmFramingInfo;
};

PACKED_STRUCT(uvc_payload_header) {
	__u8 bHeaderLength;
	__u8 bmHeaderInfo;
	__le32 dwPresentationTime;
	__le16 wSofCounter;
	__le32 scrSourceClock;
};

static void
rift_sensor_uvc_stream_release_frame(ohmd_video_frame *frame, rift_sensor_uvc_stream *stream);

int rift_sensor_uvc_set_cur(libusb_device_handle *dev, uint8_t interface, uint8_t entity,
		uint8_t selector, void *data, uint16_t wLength)
{
	uint8_t bmRequestType = LIBUSB_REQUEST_TYPE_CLASS |
		LIBUSB_RECIPIENT_INTERFACE;
	uint8_t bRequest = SET_CUR;
	uint16_t wValue = selector << 8;
	uint16_t wIndex = entity << 8 | interface;
	unsigned int timeout = TIMEOUT;
	int ret;

	ret = libusb_control_transfer(dev, bmRequestType, bRequest, wValue,
			wIndex, data, wLength, timeout);
	if (ret < 0) {
		fprintf(stderr, "failed to transfer SET CUR %u %u: %d %d %m\n",
			entity, selector, ret, errno);
	}
	return ret;
}

int rift_sensor_uvc_get_cur(libusb_device_handle *dev, uint8_t interface, uint8_t entity,
		uint8_t selector, void *data, uint16_t wLength)
{
	uint8_t bmRequestType = 0x80 | LIBUSB_REQUEST_TYPE_CLASS |
		LIBUSB_RECIPIENT_INTERFACE;
	uint8_t bRequest = GET_CUR;
	uint16_t wValue = selector << 8;
	uint16_t wIndex = entity << 8 | interface;
	unsigned int timeout = TIMEOUT;
	int ret;

	ret = libusb_control_transfer(dev, bmRequestType, bRequest, wValue,
			wIndex, data, wLength, timeout);
	if (ret < 0) {
		fprintf(stderr, "failed to transfer GET CUR %u %u: %d %d %m\n",
			entity, selector, ret, errno);
	}
	return ret;
}

void process_payload(struct rift_sensor_uvc_stream *stream, unsigned char *payload,
		     size_t len)
{
	struct uvc_payload_header *h = (struct uvc_payload_header *)payload;
	int payload_len;
	int frame_id;
	uint32_t pts = (uint32_t)(-1);
#if VERBOSE_DEBUG
	uint32_t scr = (uint32_t)(-1);
	bool have_scr;
#endif
	bool error, have_pts, is_eof;

	if (len == 0)
		return;
	if (len == 12)
		return;

	if (h->bHeaderLength != 12) {
		printf("invalid header: len %u/%u\n", h->bHeaderLength, (uint32_t) len);
		return;
	}

	payload += h->bHeaderLength;
	payload_len = len - h->bHeaderLength;
	frame_id = h->bmHeaderInfo & 0x01;
	is_eof = h->bmHeaderInfo & 0x02;
	have_pts = h->bmHeaderInfo & 0x04;
#if VERBOSE_DEBUG
	have_scr = h->bmHeaderInfo & 0x08;
#endif
	error = h->bmHeaderInfo & 0x40;

	if (error) {
		printf("UVC frame error\n");
		return;
	}

	if (have_pts) {
		pts = __le32_to_cpu(h->dwPresentationTime);
		if (stream->frame_collected != 0 && pts != stream->cur_pts) {
			printf("UVC PTS changed in-frame at %u bytes. Lost %u ms\n",
			    stream->frame_collected,
			    (pts - stream->cur_pts * 1000) / RIFT_SENSOR_CLOCK_FREQ);
			stream->cur_pts = pts;
		}
	}
#if VERBOSE_DEBUG
	if (have_scr) {
		scr = __le32_to_cpu(h->scrSourceClock);
	}
#endif

	if (frame_id != stream->frame_id) {
		struct timespec ts;
		uint64_t time;

		if (stream->frame_collected > 0) {
			printf("UVC Dropping short frame: %u < %u (%d lost)\n",
			    stream->frame_collected, stream->frame_size,
			    stream->frame_size - stream->frame_collected);
		}

		/* Start of new frame */
		clock_gettime(CLOCK_MONOTONIC, &ts);
		time = ts.tv_sec * 1000000000 + ts.tv_nsec;

		/* Get a frame to capture into */
		if (stream->cur_frame == NULL) {
			ohmd_lock_mutex(stream->frames_lock);
			if (stream->n_free_frames > 0) {
				stream->n_free_frames--;
				stream->cur_frame = stream->free_frames[stream->n_free_frames];
			} else
				stream->cur_frame = NULL;
			ohmd_unlock_mutex(stream->frames_lock);
		}

#if VERBOSE_DEBUG
		int64_t dt;
		if (stream->cur_frame)
			dt = time - stream->cur_frame->start_ts;
#endif

		stream->frame_id = frame_id;
		stream->cur_pts = pts;
		stream->frame_collected = 0;
		stream->skip_frame = false;

		if (stream->cur_frame == NULL) {
			LOGW("No frame provided for pixel data. Skipping frame");
			stream->skip_frame = true;
		}

		ohmd_video_frame *frame = stream->cur_frame;

		if (frame) {
			assert (frame->data_size == stream->frame_size);
			frame->start_ts = time;
			frame->pts = pts;
			frame->stride = stream->stride;
			frame->width = stream->width;
			frame->height = stream->height;
		}

#if VERBOSE_DEBUG
		printf ("UVC dt %f PTS %f SCR %f delta %d\n",
		    (double) (dt) / (1000000000.0),
		    (double) (pts) / RIFT_SENSOR_CLOCK_FREQ,
		    (double) (scr) / RIFT_SENSOR_CLOCK_FREQ,
		    scr - pts);
#endif
	}

	if (stream->skip_frame || stream->cur_frame == NULL)
		return;

	if (stream->frame_collected + payload_len > stream->frame_size) {
		printf("UVC frame buffer overflow: %u + %u > %u\n",
		       stream->frame_collected, payload_len, stream->frame_size);
		return;
	}

	memcpy(stream->cur_frame->data + stream->frame_collected, payload,
	       payload_len);
	stream->frame_collected += payload_len;

	if (stream->frame_collected == stream->frame_size) {
		if (stream->frame_cb) {
			stream->frame_cb(stream, stream->cur_frame, stream->frame_cb_data);
			stream->cur_frame = NULL;
		}
		stream->frame_collected = 0;
	}

	if (is_eof) {
		/* Always restart a frame after eof.
		 * CV1 sensor never seems to set this
		 * bit, but others might in the future */
		stream->frame_collected = 0;
	}
}

static void iso_transfer_cb(struct libusb_transfer *transfer)
{
	struct rift_sensor_uvc_stream *stream = transfer->user_data;
	int ret;
	int i;

	/* Handle error conditions */
	if (transfer->status != LIBUSB_TRANSFER_COMPLETED) {
		if (transfer->status != LIBUSB_TRANSFER_CANCELLED)
			printf("transfer error: %u\n", transfer->status);
		stream->active_transfers--;
		return;
	}

	if (!stream->video_running) {
		/* Not resubmitting. Reduce transfer count */
		stream->active_transfers--;
		return;
	}

	/* Handle contained isochronous packets */
	for (i = 0; i < transfer->num_iso_packets; i++) {
		unsigned char *payload;
		size_t payload_len;

		payload = libusb_get_iso_packet_buffer_simple(transfer, i);
		payload_len = transfer->iso_packet_desc[i].actual_length;
		process_payload(stream, payload, payload_len);
	}

	/* Resubmit transfer */
	for (i = 0; i < 5; i++) {
		/* Sometimes this fails, and we retry */
		ret = libusb_submit_transfer(transfer);
		if (ret >= 0)
			break;
		/* Sleep 0.5ms between retries */
		ohmd_sleep(0.0005);
	}

	if (ret < 0) {
		/* FIXME: Close and re-open this sensor */
		printf("failed to resubmit after %d attempts\n", i);
		stream->active_transfers--;
	}
	else if (i > 0) {
		printf("resubmitted xfer after %d attempts\n", i+1);
	}
}

int rift_sensor_uvc_stream_setup (ohmd_context* ohmd_ctx,
		libusb_context *usb_ctx, libusb_device_handle *devh,
		struct rift_sensor_uvc_stream *stream)
{
	libusb_device *dev = libusb_get_device(devh);
	struct libusb_device_descriptor desc;
	struct uvc_probe_commit_control control = {
		.bFormatIndex = 1,
		.bFrameIndex = 1,
	};
	int alt_setting;
	int ret;
	int num_packets;
	int packet_size;

	stream->frames_lock = ohmd_create_mutex(ohmd_ctx);
	stream->ohmd_ctx = ohmd_ctx;
	stream->usb_ctx = usb_ctx;
	stream->devh = devh;
	stream->video_running = false;

	ret = libusb_set_auto_detach_kernel_driver(devh, 1);
	if (ret < 0) {
		printf("could not detach uvcvideo driver\n");
		return ret;
	}

	ret = libusb_claim_interface(devh, 0);
	if (ret < 0) {
		printf("could not claim control interface\n");
		return ret;
	}

	ret = libusb_claim_interface(devh, 1);
	if (ret < 0) {
		printf("could not claim UVC data interface\n");
		return ret;
	}

	ret = libusb_get_device_descriptor(dev, &desc);
	if (ret < 0)
		return ret;

	switch (desc.idProduct) {
	case DK2_PID:
		control.dwFrameInterval = __cpu_to_le32(166666);
		control.dwMaxVideoFrameSize = __cpu_to_le32(752 * 480);
		control.dwMaxPayloadTransferSize = __cpu_to_le16(3000);

		stream->stride = 752;
		stream->width = 752;
		stream->height = 480;

		num_packets = 32;
		packet_size = 3060;
		alt_setting = 7;

		esp570_setup_unknown_3(devh);
		break;
	case CV1_PID:
		control.bFrameIndex = 4;
		control.dwFrameInterval = __cpu_to_le32(192000);
		control.dwMaxVideoFrameSize = __cpu_to_le32(RIFT_SENSOR_FRAME_SIZE);
		control.dwMaxPayloadTransferSize = __cpu_to_le16(3072);
		control.dwClockFrequency = __cpu_to_le32(RIFT_SENSOR_CLOCK_FREQ);

		stream->stride = RIFT_SENSOR_WIDTH;
		stream->width = RIFT_SENSOR_WIDTH;
		stream->height = RIFT_SENSOR_HEIGHT;

		packet_size = 16384;
		alt_setting = 2;
		break;
	default:
		printf("Unknown / unhandled USB device VID/PID 0x%04x / 0x%04x\n",
			desc.idVendor, desc.idProduct);
		return -1;
	}

	ret = rift_sensor_uvc_set_cur(devh, 1, 0, VS_PROBE_CONTROL, &control,
			sizeof control);
	if (ret < 0)
		return ret;

	ret = rift_sensor_uvc_get_cur(devh, 1, 0, VS_PROBE_CONTROL, &control,
			sizeof control);
	if (ret < 0) {
		printf("failed to get PROBE\n");
		return ret;
	}

	ret = rift_sensor_uvc_set_cur(devh, 1, 0, VS_COMMIT_CONTROL, &control,
			sizeof control);
	if (ret < 0) {
		printf("failed to set COMMIT\n");
		return ret;
	}

	control.dwFrameInterval = __le32_to_cpu(control.dwFrameInterval);
	control.wDelay = __le16_to_cpu(control.wDelay);
	control.dwMaxVideoFrameSize = __le32_to_cpu(control.dwMaxVideoFrameSize);
	control.dwClockFrequency = __le32_to_cpu(control.dwClockFrequency);

	control.dwMaxPayloadTransferSize = __le32_to_cpu(control.dwMaxPayloadTransferSize);

	ret = libusb_set_interface_alt_setting(devh, 1, alt_setting);
	if (ret) {
		printf("failed to set interface alt setting\n");
		return ret;
	}

	stream->frame_size = stream->stride * stream->height;

	num_packets = (stream->frame_size + packet_size - 1) / packet_size;
	stream->num_transfers = (num_packets + 31) / 32;
	num_packets = num_packets / stream->num_transfers;

	stream->transfer = ohmd_alloc(ohmd_ctx, stream->num_transfers * sizeof(*stream->transfer));
	if (!stream->transfer)
		return -ENOMEM;

	for (int i = 0; i < stream->num_transfers; i++) {
		stream->transfer[i] = libusb_alloc_transfer(num_packets);
		if (!stream->transfer[i]) {
			fprintf(stderr, "failed to allocate isochronous transfer\n");
			return -ENOMEM;
		}

		uint8_t bEndpointAddress = 0x81;
		int transfer_size = num_packets * packet_size;
		void *buf = malloc(transfer_size);
		stream->transfer[i]->flags |= LIBUSB_TRANSFER_FREE_BUFFER;

		libusb_fill_iso_transfer(stream->transfer[i], devh,
					 bEndpointAddress, buf, transfer_size,
					 num_packets, iso_transfer_cb, stream,
					 TIMEOUT);
		libusb_set_iso_packet_lengths(stream->transfer[i], packet_size);
	}

	return 0;
}

int rift_sensor_uvc_stream_start(rift_sensor_uvc_stream *stream, uint8_t min_frames,
	rift_sensor_uvc_stream_frame_cb frame_cb, void *frame_cb_data)
{
	int ret, i;

	assert (!stream->video_running);
	stream->video_running = true;

	stream->cur_frame = NULL;

	stream->frame_cb = frame_cb;
	stream->frame_cb_data = frame_cb_data;

	/* Allocate frames and put on the free list */
	stream->alloced_frames = ohmd_alloc(stream->ohmd_ctx, min_frames * sizeof (ohmd_video_frame *));
	stream->free_frames = ohmd_alloc(stream->ohmd_ctx, min_frames * sizeof (ohmd_video_frame *));

	for (i = 0; i < min_frames; i++) {
		ohmd_video_frame *frame = ohmd_alloc(stream->ohmd_ctx, sizeof (ohmd_video_frame));
		frame->data = ohmd_alloc(stream->ohmd_ctx, stream->frame_size);
		frame->data_size = stream->frame_size;

		frame->releasefn = (ohmd_video_frame_release_func) rift_sensor_uvc_stream_release_frame;
		frame->owner = stream;
		stream->alloced_frames[i] = stream->free_frames[i] = frame;
	}
	stream->n_free_frames = stream->n_alloced_frames = min_frames;

	/* Submit transfers */
	for (int i = 0; i < stream->num_transfers; i++) {
		ret = libusb_submit_transfer(stream->transfer[i]);
		if (ret < 0) {
			fprintf(stderr, "failed to submit iso transfer %d. Error %d\n", i, ret);
			stream->active_transfers = i;

			rift_sensor_uvc_stream_stop(stream);
			return ret;
		}
	}

	stream->active_transfers = stream->num_transfers;
	return 0;
}

int rift_sensor_uvc_stream_stop(struct rift_sensor_uvc_stream *stream)
{
	int ret;
	int i;

	ret = libusb_set_interface_alt_setting(stream->devh, 1, 0);
	if (ret)
		return ret;

	libusb_lock_event_waiters(stream->usb_ctx);
	stream->video_running = false;

	/* Wait for active transfers to finish */
	while (stream->active_transfers > 0) {
		ret = libusb_wait_for_event(stream->usb_ctx, NULL);
		if (ret)
			break;
	}
	libusb_unlock_event_waiters(stream->usb_ctx);

	/* Free frames */
	for (i = 0; i < stream->n_alloced_frames; i++) {
		ohmd_video_frame *frame = stream->alloced_frames[i];
		free(frame->data);
		free(frame);
	}
	free(stream->alloced_frames);
	free(stream->free_frames);

	return 0;
}

int rift_sensor_uvc_stream_clear (struct rift_sensor_uvc_stream *stream)
{
	int i;
	assert(!stream->video_running);

	if (stream->transfer != NULL) {
		for (i = 0; i < stream->num_transfers; i++) {
			if (stream->transfer[i] != NULL) {
				libusb_free_transfer(stream->transfer[i]);
				stream->transfer[i] = NULL;
			}
		}
		free(stream->transfer);
		stream->transfer = NULL;
	}

	if (stream->frames_lock != NULL) {
		ohmd_destroy_mutex(stream->frames_lock);
		stream->frames_lock = NULL;
	}

	return 0;
}

static void
rift_sensor_uvc_stream_release_frame(ohmd_video_frame *frame, rift_sensor_uvc_stream *stream)
{
	assert(frame->owner == stream);
	assert(stream->n_free_frames < stream->n_alloced_frames);

	/* Put the frame back on the free queue */
	ohmd_lock_mutex(stream->frames_lock);
	stream->free_frames[stream->n_free_frames++] = frame;
	ohmd_unlock_mutex(stream->frames_lock);
}
