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
 * Uploads an image to a staging service via multipart/form-data POST and
 * returns the resulting URL. The service (paste.boxlabs.uk and compatible)
 * expects a file field named "images[]" and an optional "strip_exif" flag,
 * and replies with JSON of the form:
 *
 *   {"results":[{"success":true,"filePath":"/img/img_xxx.png", ...}]}
 *   {"results":[{"success":false,"error":"..."}]}
 *
 * The returned filePath may be relative, so it is resolved against the
 * endpoint to an absolute URL. HTTP is done with libsoup3 (Linux/macOS) or
 * libcurl (Windows), matching network-icon.c.
 */

#include "config.h"

#include <string.h>
#include <glib.h>

#ifdef USE_LIBSOUP
#include <libsoup/soup.h>
#include <jansson.h>
#endif

#ifdef USE_LIBCURL
#include <curl/curl.h>
#include <jansson.h>
#endif

#include "image-upload.h"
#include "poxchat.h"
#include "poxchatc.h"

/* ---- Upload context (shared by both backends) ---- */

typedef struct {
	GBytes *image;          /* image bytes (owned) */
	char *filename;         /* suggested upload name (owned) */
	const char *mime;       /* MIME type (static literal, not owned) */
	char *endpoint;         /* upload URL (owned) */
	gboolean strip_exif;    /* whether THIS attempt requests EXIF stripping */
	int attempt;            /* 0 = first try, 1 = strip-disabled retry */
	image_upload_cb cb;
	void *user_data;
} upload_ctx;

static void upload_send (upload_ctx *ctx);

static const char *
mime_from_filename (const char *name)
{
	const char *dot = name ? strrchr (name, '.') : NULL;

	if (!dot)
		return "application/octet-stream";
	if (!g_ascii_strcasecmp (dot, ".png"))
		return "image/png";
	if (!g_ascii_strcasecmp (dot, ".jpg") || !g_ascii_strcasecmp (dot, ".jpeg"))
		return "image/jpeg";
	if (!g_ascii_strcasecmp (dot, ".gif"))
		return "image/gif";
	if (!g_ascii_strcasecmp (dot, ".webp"))
		return "image/webp";
	return "application/octet-stream";
}

static void
upload_ctx_free (upload_ctx *ctx)
{
	if (ctx->image)
		g_bytes_unref (ctx->image);
	g_free (ctx->filename);
	g_free (ctx->endpoint);
	g_free (ctx);
}

/* Report the final outcome and release the context. */
static void
upload_finish (upload_ctx *ctx, const char *url, const char *error)
{
	ctx->cb (url, error, ctx->user_data);
	upload_ctx_free (ctx);
}

#if defined(USE_LIBSOUP) || defined(USE_LIBCURL)

/* Does this server error look like a metadata-strip failure? If so, we retry
 * once without strip_exif so the upload still goes through. */
static gboolean
error_is_strip_failure (const char *err)
{
	char *low;
	gboolean hit;

	if (!err)
		return FALSE;

	low = g_ascii_strdown (err, -1);
	hit = (strstr (low, "strip") != NULL) || (strstr (low, "exif") != NULL);
	g_free (low);
	return hit;
}

/* Parse the JSON reply and either finish (success/error) or kick off the
 * EXIF-fallback retry. `body`/`body_len` is the raw response (not necessarily
 * NUL-terminated); `transport_error` is set when the HTTP request itself
 * failed. Always either finishes the context or re-sends it. */
static void
upload_handle_response (upload_ctx *ctx, const char *body, gsize body_len,
                        const char *transport_error)
{
	json_t *root, *results, *r0, *succ;
	json_error_t jerr;

	if (transport_error)
	{
		upload_finish (ctx, NULL, transport_error);
		return;
	}

	if (!body || body_len == 0)
	{
		upload_finish (ctx, NULL, "Empty server response");
		return;
	}

	root = json_loadb (body, body_len, 0, &jerr);
	if (!root)
	{
		upload_finish (ctx, NULL, "Invalid server response");
		return;
	}

	results = json_object_get (root, "results");
	r0 = (results && json_is_array (results)) ? json_array_get (results, 0)
	                                          : NULL;
	if (!r0 || !json_is_object (r0))
	{
		json_decref (root);
		upload_finish (ctx, NULL, "Unexpected server response");
		return;
	}

	succ = json_object_get (r0, "success");
	if (json_is_true (succ))
	{
		const char *file_path = json_string_value (
		    json_object_get (r0, "filePath"));

		if (file_path && file_path[0])
		{
			/* filePath is usually relative ("/img/..."); resolve it
			 * against the endpoint to get an absolute URL. */
			char *abs = g_uri_resolve_relative (ctx->endpoint, file_path,
			                                    G_URI_FLAGS_NONE, NULL);
			upload_finish (ctx, abs ? abs : file_path, NULL);
			g_free (abs);
		}
		else
		{
			upload_finish (ctx, NULL, "Server returned no file path");
		}
		json_decref (root);
		return;
	}

	/* Failure path */
	{
		const char *errmsg = json_string_value (json_object_get (r0, "error"));

		/* EXIF auto-fallback: if we asked the server to strip metadata and
		 * that is what failed, retry once with stripping disabled. */
		if (ctx->strip_exif && ctx->attempt == 0
		    && error_is_strip_failure (errmsg))
		{
			json_decref (root);
			ctx->strip_exif = FALSE;
			ctx->attempt = 1;
			upload_send (ctx);
			return;
		}

		upload_finish (ctx, NULL, errmsg ? errmsg : "Upload failed");
	}
	json_decref (root);
}

#endif /* USE_LIBSOUP || USE_LIBCURL */

/* ---- libsoup3 backend ---- */

#ifdef USE_LIBSOUP

static void
upload_soup_cb (GObject *source, GAsyncResult *result, gpointer user_data)
{
	upload_ctx *ctx = (upload_ctx *)user_data;
	SoupSession *session = SOUP_SESSION (source);
	GBytes *body;
	GError *error = NULL;
	const char *data;
	gsize len;

	body = soup_session_send_and_read_finish (session, result, &error);
	g_object_unref (session);

	if (error)
	{
		upload_handle_response (ctx, NULL, 0, error->message);
		g_error_free (error);
		if (body)
			g_bytes_unref (body);
		return;
	}

	data = g_bytes_get_data (body, &len);
	upload_handle_response (ctx, data, len, NULL);
	g_bytes_unref (body);
}

static void
upload_send (upload_ctx *ctx)
{
	SoupSession *session;
	SoupMultipart *multipart;
	SoupMessage *msg;

	multipart = soup_multipart_new ("multipart/form-data");
	soup_multipart_append_form_file (multipart, "images[]", ctx->filename,
	                                 ctx->mime, ctx->image);
	if (ctx->strip_exif)
		soup_multipart_append_form_string (multipart, "strip_exif", "1");

	msg = soup_message_new_from_multipart (ctx->endpoint, multipart);
	soup_multipart_free (multipart);

	if (!msg)
	{
		upload_finish (ctx, NULL, "Invalid upload URL");
		return;
	}

	session = soup_session_new ();
	soup_session_send_and_read_async (session, msg, G_PRIORITY_DEFAULT, NULL,
	                                  upload_soup_cb, ctx);
	g_object_unref (msg);
}

#elif defined(USE_LIBCURL)

/* ---- libcurl backend ---- */

typedef struct {
	char *data;
	size_t size;
} curl_buf;

static size_t
upload_curl_write_cb (void *contents, size_t size, size_t nmemb, void *userp)
{
	size_t realsize = size * nmemb;
	curl_buf *buf = (curl_buf *)userp;
	char *ptr;

	ptr = g_realloc (buf->data, buf->size + realsize + 1);
	if (!ptr)
		return 0;

	buf->data = ptr;
	memcpy (&(buf->data[buf->size]), contents, realsize);
	buf->size += realsize;
	buf->data[buf->size] = '\0';
	return realsize;
}

typedef struct {
	upload_ctx *ctx;
	curl_buf response;
	CURLcode result;
	char *error_msg;
} curl_upload_ctx;

/* Main thread: feed the response to the shared handler, free per-attempt
 * state. Note ctx lifetime is owned by upload_handle_response (it may retry),
 * so we never free ctx here. */
static gboolean
upload_curl_complete_idle (gpointer user_data)
{
	curl_upload_ctx *cctx = (curl_upload_ctx *)user_data;

	if (cctx->result != CURLE_OK)
		upload_handle_response (cctx->ctx, NULL, 0, cctx->error_msg);
	else
		upload_handle_response (cctx->ctx, cctx->response.data,
		                        cctx->response.size, NULL);

	g_free (cctx->error_msg);
	g_free (cctx->response.data);
	g_free (cctx);
	return G_SOURCE_REMOVE;
}

static gpointer
upload_curl_thread (gpointer user_data)
{
	curl_upload_ctx *cctx = (curl_upload_ctx *)user_data;
	upload_ctx *ctx = cctx->ctx;
	CURL *curl;
	curl_mime *mime;
	curl_mimepart *part;
	gconstpointer img_data;
	gsize img_len;

	curl = curl_easy_init ();
	if (!curl)
	{
		cctx->result = CURLE_FAILED_INIT;
		cctx->error_msg = g_strdup ("Failed to init libcurl");
		g_idle_add (upload_curl_complete_idle, cctx);
		return NULL;
	}

	img_data = g_bytes_get_data (ctx->image, &img_len);

	mime = curl_mime_init (curl);
	part = curl_mime_addpart (mime);
	curl_mime_name (part, "images[]");
	curl_mime_filename (part, ctx->filename);
	curl_mime_type (part, ctx->mime);
	curl_mime_data (part, (const char *)img_data, (size_t)img_len);

	if (ctx->strip_exif)
	{
		part = curl_mime_addpart (mime);
		curl_mime_name (part, "strip_exif");
		curl_mime_data (part, "1", CURL_ZERO_TERMINATED);
	}

	curl_easy_setopt (curl, CURLOPT_URL, ctx->endpoint);
	curl_easy_setopt (curl, CURLOPT_MIMEPOST, mime);
	curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION, upload_curl_write_cb);
	curl_easy_setopt (curl, CURLOPT_WRITEDATA, &cctx->response);
	curl_easy_setopt (curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt (curl, CURLOPT_TIMEOUT, 60L);
#ifdef WIN32
	curl_easy_setopt (curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif

	cctx->result = curl_easy_perform (curl);
	curl_mime_free (mime);
	curl_easy_cleanup (curl);

	if (cctx->result != CURLE_OK)
		cctx->error_msg = g_strdup_printf ("Upload failed: %s",
		                                   curl_easy_strerror (cctx->result));

	g_idle_add (upload_curl_complete_idle, cctx);
	return NULL;
}

static void
upload_send (upload_ctx *ctx)
{
	curl_upload_ctx *cctx = g_new0 (curl_upload_ctx, 1);
	cctx->ctx = ctx;
	g_thread_unref (g_thread_new ("image-upload", upload_curl_thread, cctx));
}

#else /* no HTTP library */

static void
upload_send (upload_ctx *ctx)
{
	upload_finish (ctx, NULL,
	               "No HTTP library available (need libsoup3 or libcurl)");
}

#endif /* USE_LIBSOUP / USE_LIBCURL */

/* ---- Public API ---- */

void
image_upload_bytes (const guint8 *data, gsize len, const char *filename,
                    image_upload_cb cb, void *user_data)
{
	upload_ctx *ctx;

	if (!cb)
		return;

	if (!prefs.hex_url_image_upload_enable)
	{
		cb (NULL, "Image upload is disabled", user_data);
		return;
	}
	if (!prefs.hex_url_image_upload[0])
	{
		cb (NULL, "No image upload endpoint configured", user_data);
		return;
	}
	if (!data || len == 0)
	{
		cb (NULL, "Empty image", user_data);
		return;
	}
	if (len > IMAGE_UPLOAD_MAX_SIZE)
	{
		cb (NULL, "Image exceeds the 10 MB upload limit", user_data);
		return;
	}

	ctx = g_new0 (upload_ctx, 1);
	ctx->image = g_bytes_new (data, len);
	ctx->filename = g_strdup ((filename && filename[0]) ? filename
	                                                    : "image.png");
	ctx->mime = mime_from_filename (ctx->filename);
	ctx->endpoint = g_strdup (prefs.hex_url_image_upload);
	ctx->strip_exif = prefs.hex_url_image_strip_exif ? TRUE : FALSE;
	ctx->attempt = 0;
	ctx->cb = cb;
	ctx->user_data = user_data;

	upload_send (ctx);
}
