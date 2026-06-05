/* PoxChat - Image staging upload
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Uploads pasted/dropped images to an image-staging service (default
 * paste.boxlabs.uk) and hands the resulting URL back to the frontend.
 * Uses libsoup3 on Linux/macOS and libcurl on Windows, mirroring
 * network-icon.c.
 */

#ifndef POXCHAT_IMAGE_UPLOAD_H
#define POXCHAT_IMAGE_UPLOAD_H

#include <glib.h>

/* Completion callback, always invoked on the main thread.
 *   url   = absolute URL of the uploaded image on success, NULL on failure.
 *   error = human-readable error message on failure, NULL on success. */
typedef void (*image_upload_cb) (const char *url, const char *error,
                                 void *user_data);

/* Upload raw image bytes to the configured staging service
 * (prefs.hex_url_image_upload). Copies the data it needs, so the caller may
 * free `data` immediately after this returns. `filename` is the suggested
 * upload name (e.g. "pasted.png"); NULL/empty falls back to a default.
 *
 * The callback fires exactly once when the upload completes or fails. It is a
 * no-op (callback reports an error) when image upload is disabled, no endpoint
 * is configured, or no HTTP backend was built in. */
void image_upload_bytes (const guint8 *data, gsize len, const char *filename,
                         image_upload_cb cb, void *user_data);

/* Maximum upload size in bytes (10 MB, matching the service limit). */
#define IMAGE_UPLOAD_MAX_SIZE (10 * 1024 * 1024)

#endif /* POXCHAT_IMAGE_UPLOAD_H */
