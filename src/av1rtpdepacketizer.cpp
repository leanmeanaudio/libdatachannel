/**
 * Copyright (c) 2024 Paul-Louis Ageneau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if RTC_ENABLE_MEDIA

#include "av1rtpdepacketizer.hpp"
#include "rtp.hpp"

namespace rtc {

/*
 * AV1 RTP payload format — aggregation header
 * See https://aomediacodec.github.io/av1-rtp-spec/#44-av1-aggregation-header
 *
 *  0 1 2 3 4 5 6 7
 * +-+-+-+-+-+-+-+-+
 * |Z|Y| W |N|-|-|-|
 * +-+-+-+-+-+-+-+-+
 *
 * Z: 1 if the first OBU element is a continuation of a fragment from the
 *    previous packet.
 * Y: 1 if the last OBU element will continue in the next packet.
 * W: Number of OBU elements in the packet (0 means each element is
 *    preceded by a length field; 1–3 means the last element has no
 *    length field and its size is inferred).
 * N: 1 if this is the first packet of a coded video sequence.
 */

const uint8_t zMask = 0b10000000;
const uint8_t yMask = 0b01000000;
const uint8_t wMask = 0b00110000;
const int     wShift = 4;

const uint8_t sevenLsbBitmask = 0b01111111;
const uint8_t msbBitmask = 0b10000000;

// Read a LEB128-encoded unsigned integer from data at offset.
// Returns the decoded value and advances offset past the encoded bytes.
static uint32_t readLeb128(const std::byte *data, size_t size, size_t &offset) {
	uint32_t value = 0;
	for (int i = 0; i < 8; i++) {
		if (offset >= size)
			break;
		auto b = std::to_integer<uint8_t>(data[offset]);
		value |= static_cast<uint32_t>(b & sevenLsbBitmask) << (i * 7);
		offset++;
		if (!(b & msbBitmask))
			break;
	}
	return value;
}

AV1RtpDepacketizer::AV1RtpDepacketizer() {}

AV1RtpDepacketizer::~AV1RtpDepacketizer() {}

message_ptr AV1RtpDepacketizer::reassemble(message_buffer &buffer) {
	if (buffer.empty())
		return nullptr;

	auto first = *buffer.begin();
	auto firstRtpHeader = reinterpret_cast<const RtpHeader *>(first->data());
	uint8_t payloadType = firstRtpHeader->payloadType();
	uint32_t timestamp = firstRtpHeader->timestamp();
	uint16_t nextSeqNumber = firstRtpHeader->seqNumber();

	binary frame;
	binary currentObu;           // accumulates fragments of a single OBU
	bool obuContinuation = false; // true when we expect a Z=1 continuation

	for (const auto &packet : buffer) {
		auto rtpHeader = reinterpret_cast<const RtpHeader *>(packet->data());
		auto seqDiff = static_cast<int16_t>(rtpHeader->seqNumber() - nextSeqNumber);
		if (seqDiff < 0)
			continue; // duplicate / old

		if (seqDiff > 0) {
			// Gap — discard any in-progress OBU and reset
			currentObu.clear();
			obuContinuation = false;
		}
		nextSeqNumber = rtpHeader->seqNumber() + 1;

		auto rtpHeaderSize = rtpHeader->getSize() + rtpHeader->getExtensionHeaderSize();
		size_t paddingSize = 0;
		if (rtpHeader->padding())
			paddingSize = std::to_integer<uint8_t>(packet->back());

		if (packet->size() <= rtpHeaderSize + paddingSize)
			continue;

		const std::byte *payloadData = packet->data() + rtpHeaderSize;
		size_t payloadSize = packet->size() - rtpHeaderSize - paddingSize;

		if (payloadSize < 1)
			continue;

		uint8_t aggHeader = std::to_integer<uint8_t>(payloadData[0]);
		bool zFlag = (aggHeader & zMask) != 0;
		bool yFlag = (aggHeader & yMask) != 0;
		int  wField = (aggHeader & wMask) >> wShift;

		size_t offset = 1; // skip aggregation header

		// If Z=0 but we were expecting a continuation, the previous OBU
		// was truncated — discard it.
		if (!zFlag && obuContinuation) {
			currentObu.clear();
			obuContinuation = false;
		}

		// Determine how many OBU elements are in this packet.
		// W=0 means each element has a preceding LEB128 length.
		// W=1..3 means exactly that many elements; the last one has no
		// length field (its length is inferred from the remaining payload).
		int obuCount = wField; // 0 means "use length fields for all"

		if (wField == 0) {
			// All elements have length fields. Parse them sequentially
			// until the payload is exhausted.
			while (offset < payloadSize) {
				size_t lenOffset = offset;
				uint32_t elemLen = readLeb128(payloadData, payloadSize, offset);
				if (offset + elemLen > payloadSize)
					break;

				bool isFirst = (lenOffset == 1);
				bool isLast = (offset + elemLen >= payloadSize);

				if (isFirst && zFlag) {
					// Continuation of previous OBU fragment
					currentObu.insert(currentObu.end(),
					                  payloadData + offset,
					                  payloadData + offset + elemLen);
				}
				else {
					// Flush any completed OBU
					if (!currentObu.empty()) {
						frame.insert(frame.end(), currentObu.begin(), currentObu.end());
						currentObu.clear();
					}
					currentObu.insert(currentObu.end(),
					                  payloadData + offset,
					                  payloadData + offset + elemLen);
				}

				if (isLast && yFlag) {
					obuContinuation = true;
				}
				else {
					frame.insert(frame.end(), currentObu.begin(), currentObu.end());
					currentObu.clear();
					obuContinuation = false;
				}

				offset += elemLen;
			}
		}
		else {
			// W = 1..3: exactly wField OBU elements.
			// First (wField - 1) elements have LEB128 length fields.
			// The last element fills the remaining payload.
			for (int i = 0; i < obuCount; i++) {
				size_t elemLen;
				if (i < obuCount - 1) {
					elemLen = readLeb128(payloadData, payloadSize, offset);
					if (offset + elemLen > payloadSize)
						break;
				}
				else {
					// Last element: remaining payload
					elemLen = payloadSize - offset;
				}

				bool isFirst = (i == 0);
				bool isLast = (i == obuCount - 1);

				if (isFirst && zFlag) {
					currentObu.insert(currentObu.end(),
					                  payloadData + offset,
					                  payloadData + offset + elemLen);
				}
				else {
					if (!currentObu.empty()) {
						frame.insert(frame.end(), currentObu.begin(), currentObu.end());
						currentObu.clear();
					}
					currentObu.insert(currentObu.end(),
					                  payloadData + offset,
					                  payloadData + offset + elemLen);
				}

				if (isLast && yFlag) {
					obuContinuation = true;
				}
				else {
					frame.insert(frame.end(), currentObu.begin(), currentObu.end());
					currentObu.clear();
					obuContinuation = false;
				}

				offset += elemLen;
			}
		}
	}

	// Flush any remaining OBU (from the last packet, where Y=0)
	if (!currentObu.empty())
		frame.insert(frame.end(), currentObu.begin(), currentObu.end());

	if (frame.empty())
		return nullptr;

	return make_message(std::move(frame), createFrameInfo(timestamp, payloadType));
}

} // namespace rtc

#endif // RTC_ENABLE_MEDIA
