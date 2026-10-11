#pragma once

#include <cstdint>
#include <mutex>
#include <optional>

// Shared by the text and file workers for one stream. Native clipboard sequence
// numbers order local copies; our own deliveries are not new local mutations.
class ClipboardCoordinator
{
public:
    struct Settlement {
        uint32_t localSequence;
        std::optional<uint32_t> hostSequence;
        bool conflict;
    };

    void initialize(uint32_t sequence)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_LocalSequence) m_LocalSequence = sequence;
    }

    bool observeLocal(uint32_t sequence, bool isText, bool textObservation = false)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_RemoteSequence == sequence || older(sequence)) return false;
        if (m_LocalSequence != sequence) {
            m_Settlement.reset();
            m_SettledSequence.reset();
            m_TextObservedSequence.reset();
            m_LocalSequence = sequence;
        }
        // The file worker can identify a potential text copy before the SDL
        // owner samples it. Only that owner's observation knows whether the
        // text is usable (for example, within the configured size bound).
        if (textObservation || m_TextObservedSequence != sequence) {
            if (textObservation) m_TextObservedSequence = sequence;
            if (m_SettledSequence != sequence) m_TextPending = isText && m_TextEnabled;
        }
        return true;
    }

    void observeRemote(uint32_t sequence)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (older(sequence)) return;
        recordRemote(sequence);
    }

    // Prepare data before calling. The callback must only perform the short,
    // native sequence-guarded publication and must not call this coordinator.
    // Holding this lock prevents a same-sequence text observation from becoming
    // pending between the incoming-file check and clipboard publication.
    template<class Publish>
    std::optional<uint32_t> publishIncoming(uint32_t expectedSequence, Publish publish)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_LocalSequence != expectedSequence || m_TextPending) return std::nullopt;
        auto sequence = publish();
        if (sequence) recordRemote(*sequence);
        return sequence;
    }

    bool textPending(uint32_t sequence)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_LocalSequence == sequence && m_TextPending;
    }

    void settleText(uint32_t sequence, std::optional<uint32_t> hostSequence, bool conflict = false)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_LocalSequence != sequence) return;
        m_TextPending = false;
        m_SettledSequence = sequence;
        m_Settlement = Settlement {sequence, hostSequence, conflict};
    }

    void disableText()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_TextEnabled = m_TextPending = false;
    }

    std::optional<Settlement> takeSettlement()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        auto result = m_Settlement;
        m_Settlement.reset();
        return result;
    }

private:
    void recordRemote(uint32_t sequence)
    {
        m_LocalSequence = m_RemoteSequence = sequence;
        m_TextPending = false;
        m_SettledSequence = sequence;
    }

    bool older(uint32_t sequence) const
    {
        // Native sequence values wrap. Streams cannot span 2^31 clipboard edits.
        return m_LocalSequence && sequence - *m_LocalSequence > 0x80000000u;
    }
    std::mutex m_Mutex;
    std::optional<uint32_t> m_LocalSequence;
    std::optional<uint32_t> m_RemoteSequence;
    std::optional<uint32_t> m_SettledSequence;
    std::optional<uint32_t> m_TextObservedSequence;
    bool m_TextEnabled = true;
    bool m_TextPending = false;
    std::optional<Settlement> m_Settlement;
};
