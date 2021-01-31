// Copyright 2020 Jan Schmidt
// SPDX-License-Identifier: BSL-1.0
/*
 * OpenHMD - Free and Open Source API and drivers for immersive technology.
 */
#ifndef __POSE_HELPER_H__
#define __POSE_HELPER_H__

#include "rift.h"
#include "rift-sensor-maths.h"
#include "rift-sensor-blobwatch.h"

#define MAX_OBJECT_LEDS 64

typedef struct {
 double left;
 double top;
 double right;
 double bottom;
} rift_rect_t;

typedef struct {
	int matched_blobs;
	int unmatched_blobs;
	int visible_leds;

	double reprojection_error;

	bool good_pose_match; /* TRUE if rift_evaluate_pose() considered this a good match */
} rift_pose_metrics;

void rift_evaluate_pose (rift_pose_metrics *score, posef *pose,
	struct blob *blobs, int num_blobs,
	int device_id, rift_led *leds, int num_leds,
	dmat3 *camera_matrix, double dist_coeffs[5], bool dist_fisheye,
	rift_rect_t *out_bounds);

void rift_evaluate_pose_with_prior (rift_pose_metrics *score, posef *pose,
	posef *pose_prior, const vec3f *pos_variance, const vec3f *rot_variance,
	struct blob *blobs, int num_blobs,
	int device_id, rift_led *leds, int num_leds,
	dmat3 *camera_matrix, double dist_coeffs[5], bool dist_fisheye,
	rift_rect_t *out_bounds);

void rift_mark_matching_blobs (posef *pose,
	struct blob *blobs, int num_blobs,
	int device_id, rift_led *leds, int num_leds,
	dmat3 *camera_matrix, double dist_coeffs[5], bool dist_fisheye);
#endif
