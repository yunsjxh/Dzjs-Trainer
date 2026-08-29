#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

struct JIYU_AV_PATTERN_SAMPLE_VIEW
{
    const unsigned char* bytes;
    std::size_t length;
    std::size_t preferredOffset;
    const unsigned char* stableMask = nullptr;
};

inline bool JiYuAvSampleByteStable(
    const JIYU_AV_PATTERN_SAMPLE_VIEW& sample,
    std::size_t offset)
{
    return sample.stableMask == nullptr || sample.stableMask[offset] != 0U;
}

inline bool JiYuAvIsDistinctiveAnchor(const unsigned char* bytes, std::size_t length)
{
    bool seen[256] = {};
    std::size_t distinct = 0;
    std::size_t run = 1;
    std::size_t longestRun = 1;
    for (std::size_t index = 0; index < length; ++index) {
        if (!seen[bytes[index]]) {
            seen[bytes[index]] = true;
            ++distinct;
        }
        if (index != 0 && bytes[index] == bytes[index - 1]) {
            longestRun = (std::max)(longestRun, ++run);
        }
        else {
            run = 1;
        }
    }
    return distinct >= (length + 1U) / 2U && longestRun <= 4U;
}

inline bool JiYuAvPatternMatchesBuffer(
    const JIYU_AV_PATTERN_SAMPLE_VIEW& sample,
    const unsigned char* pattern,
    const unsigned char* mask,
    std::size_t patternLength)
{
    if (!sample.bytes || !pattern || !mask || patternLength == 0 || sample.length < patternLength) {
        return false;
    }
    for (std::size_t start = 0; start <= sample.length - patternLength; ++start) {
        bool matched = true;
        for (std::size_t index = 0; index < patternLength; ++index) {
            if ((sample.bytes[start + index] & mask[index]) != (pattern[index] & mask[index])) {
                matched = false;
                break;
            }
        }
        if (matched) return true;
    }
    return false;
}

inline bool JiYuAvBuildSharedPattern(
    const JIYU_AV_PATTERN_SAMPLE_VIEW* samples,
    std::size_t sampleCount,
    unsigned char* pattern,
    unsigned char* mask,
    std::size_t outputCapacity,
    std::size_t* outputLength,
    std::size_t* exactByteCount)
{
    constexpr std::size_t patternLength = 48;
    constexpr std::size_t desiredPrefix = 16;
    constexpr std::size_t maximumCandidates = 4096;
    constexpr std::size_t anchorLengths[] = { 12, 10, 8 };

    if (outputLength) *outputLength = 0;
    if (exactByteCount) *exactByteCount = 0;
    if (!samples || sampleCount == 0 || !pattern || !mask || outputCapacity < patternLength) return false;
    for (std::size_t index = 0; index < sampleCount; ++index) {
        if (!samples[index].bytes || samples[index].length < patternLength) return false;
    }

    std::vector<std::size_t> bestPositions(sampleCount, 0);
    std::size_t bestExact = 0;
    std::size_t bestScore = 0;
    const JIYU_AV_PATTERN_SAMPLE_VIEW& base = samples[0];

    if (sampleCount == 1) {
        const std::size_t start = (std::min)(base.preferredOffset, base.length - patternLength);
        std::size_t exact = 0;
        for (std::size_t offset = 0; offset < patternLength; ++offset) {
            const bool stable = JiYuAvSampleByteStable(base, start + offset);
            pattern[offset] = stable ? base.bytes[start + offset] : 0U;
            mask[offset] = stable ? 0xFFU : 0U;
            if (stable) ++exact;
        }
        if (exact < 16) return false;
        if (outputLength) *outputLength = patternLength;
        if (exactByteCount) *exactByteCount = exact;
        return true;
    }

    for (std::size_t anchorLength : anchorLengths) {
        const std::size_t firstAnchor = desiredPrefix;
        const std::size_t lastAnchor = base.length - (patternLength - desiredPrefix);
        if (lastAnchor < firstAnchor || lastAnchor + anchorLength > base.length) continue;
        const std::size_t candidateSpace = lastAnchor - firstAnchor;
        const std::size_t candidateCount = (std::min)(maximumCandidates, candidateSpace + 1U);

        for (std::size_t candidate = 0; candidate < candidateCount; ++candidate) {
            const std::size_t baseAnchor = firstAnchor +
                (candidateCount == 1 ? 0 : candidate * candidateSpace / (candidateCount - 1));
            bool baseAnchorStable = true;
            for (std::size_t index = 0; index < anchorLength; ++index) {
                if (!JiYuAvSampleByteStable(base, baseAnchor + index)) {
                    baseAnchorStable = false;
                    break;
                }
            }
            if (!baseAnchorStable) continue;
            if (!JiYuAvIsDistinctiveAnchor(base.bytes + baseAnchor, anchorLength)) continue;

            std::vector<std::size_t> positions(sampleCount, 0);
            positions[0] = baseAnchor;
            bool common = true;
            for (std::size_t sampleIndex = 1; sampleIndex < sampleCount; ++sampleIndex) {
                const JIYU_AV_PATTERN_SAMPLE_VIEW& sample = samples[sampleIndex];
                const std::size_t lastSampleAnchor = sample.length - (patternLength - desiredPrefix);
                const unsigned char* searchBegin = sample.bytes + desiredPrefix;
                const unsigned char* searchEnd = sample.bytes + lastSampleAnchor + anchorLength;
                const unsigned char* found = searchEnd;
                const unsigned char* cursor = searchBegin;
                while (cursor < searchEnd) {
                    const unsigned char* candidateMatch = std::search(
                        cursor,
                        searchEnd,
                        base.bytes + baseAnchor,
                        base.bytes + baseAnchor + anchorLength);
                    if (candidateMatch == searchEnd) break;
                    const std::size_t candidateOffset = static_cast<std::size_t>(candidateMatch - sample.bytes);
                    bool stable = true;
                    for (std::size_t anchorIndex = 0; anchorIndex < anchorLength; ++anchorIndex) {
                        if (!JiYuAvSampleByteStable(sample, candidateOffset + anchorIndex)) {
                            stable = false;
                            break;
                        }
                    }
                    if (stable) {
                        found = candidateMatch;
                        break;
                    }
                    cursor = candidateMatch + 1;
                }
                if (found == searchEnd) {
                    common = false;
                    break;
                }
                positions[sampleIndex] = static_cast<std::size_t>(found - sample.bytes);
            }
            if (!common) continue;

            bool seenStable[256] = {};
            std::size_t exact = 0;
            std::size_t distinctStable = 0;
            std::size_t paddingBytes = 0;
            std::size_t paddingRun = 0;
            std::size_t longestPaddingRun = 0;
            for (std::size_t offset = 0; offset < patternLength; ++offset) {
                const unsigned char value = base.bytes[positions[0] - desiredPrefix + offset];
                bool same = JiYuAvSampleByteStable(
                    base, positions[0] - desiredPrefix + offset);
                for (std::size_t sampleIndex = 1; same && sampleIndex < sampleCount; ++sampleIndex) {
                    const std::size_t sampleOffset = positions[sampleIndex] - desiredPrefix + offset;
                    if (!JiYuAvSampleByteStable(samples[sampleIndex], sampleOffset) ||
                        samples[sampleIndex].bytes[sampleOffset] != value) {
                        same = false;
                        break;
                    }
                }
                if (same) {
                    ++exact;
                    if (!seenStable[value]) {
                        seenStable[value] = true;
                        ++distinctStable;
                    }
                    if (value == 0x00U || value == 0x90U || value == 0xCCU) {
                        ++paddingBytes;
                        longestPaddingRun = (std::max)(longestPaddingRun, ++paddingRun);
                    }
                    else paddingRun = 0;
                }
                else paddingRun = 0;
            }
            if (exact < 16 || distinctStable < 8 || longestPaddingRun > 4) continue;
            const std::size_t score = exact * 16U + distinctStable * 4U - paddingBytes * 3U;
            if (score > bestScore) {
                bestScore = score;
                bestExact = exact;
                bestPositions = positions;
            }
            if (bestExact >= 40 && distinctStable >= 16 && longestPaddingRun <= 2) break;
        }
        if (bestExact >= 40 && bestScore >= 704U) break;
    }

    if (bestExact < 16) return false;
    for (std::size_t offset = 0; offset < patternLength; ++offset) {
        const unsigned char value = base.bytes[bestPositions[0] - desiredPrefix + offset];
        bool same = JiYuAvSampleByteStable(
            base, bestPositions[0] - desiredPrefix + offset);
        for (std::size_t sampleIndex = 1; same && sampleIndex < sampleCount; ++sampleIndex) {
            const std::size_t sampleOffset = bestPositions[sampleIndex] - desiredPrefix + offset;
            if (!JiYuAvSampleByteStable(samples[sampleIndex], sampleOffset) ||
                samples[sampleIndex].bytes[sampleOffset] != value) {
                same = false;
                break;
            }
        }
        pattern[offset] = same ? value : 0U;
        mask[offset] = same ? 0xFFU : 0U;
    }

    for (std::size_t sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
        if (!JiYuAvPatternMatchesBuffer(samples[sampleIndex], pattern, mask, patternLength)) return false;
    }
    if (outputLength) *outputLength = patternLength;
    if (exactByteCount) *exactByteCount = bestExact;
    return true;
}
