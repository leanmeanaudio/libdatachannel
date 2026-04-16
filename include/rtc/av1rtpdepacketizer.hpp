/**
 * Copyright (c) 2024 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#ifndef RTC_AV1_RTP_DEPACKETIZER_H
#define RTC_AV1_RTP_DEPACKETIZER_H

#if RTC_ENABLE_MEDIA

#include "common.hpp"
#include "message.hpp"
#include "rtpdepacketizer.hpp"

namespace rtc {

/// RTP depacketization for AV1
class RTC_CPP_EXPORT AV1RtpDepacketizer final : public VideoRtpDepacketizer {
public:
	AV1RtpDepacketizer();
	~AV1RtpDepacketizer();

private:
	message_ptr reassemble(message_buffer &buffer) override;
};

} // namespace rtc

#endif // RTC_ENABLE_MEDIA

#endif /* RTC_AV1_RTP_DEPACKETIZER_H */
