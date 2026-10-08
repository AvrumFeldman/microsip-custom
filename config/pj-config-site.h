// SPDX-License-Identifier: GPL-2.0-or-later
// Voice-only custom build; use the same file for PJSIP and the application.
#pragma once
#define PJMEDIA_HAS_VIDEO 0
#define PJMEDIA_HAS_OPUS_CODEC 1
#define PJMEDIA_HAS_WEBRTC_AEC 1
#define PJ_HAS_SSL_SOCK 1
#define PJMEDIA_HAS_SRTP 1
#define PJMEDIA_SRTP_HAS_DTLS 1
#define PJSUA_MAX_CALLS 32
#define PJMEDIA_AUDIO_DEV_HAS_WMME 1
#define PJMEDIA_AUDIO_DEV_HAS_WASAPI 0
