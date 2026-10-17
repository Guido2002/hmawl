#include "native_dispatch.h"

#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

std::string Address(uint32_t value) {
    std::ostringstream text;
    text << "0x" << std::hex << std::uppercase << std::setw(8)
         << std::setfill('0') << value;
    return text.str();
}

} // namespace

void NativeDispatcher::CheckCode(const PpcState& state,
                                 const NativeTranslation& translation) const {
    for (size_t i = 0; i < translation.expectedWords.size(); ++i) {
        const uint32_t address = translation.address + static_cast<uint32_t>(i * 4u);
        if (ppc_read_u32(state, address) != translation.expectedWords[i]) {
            throw std::runtime_error("Native translation code mismatch at " + Address(address) + ".");
        }
    }
}

void NativeDispatcher::Register(
    uint32_t moduleId, const PpcState& state,
    const std::vector<NativeTranslation>& translations) {
    if (executing_) {
        throw std::runtime_error("Cannot change native bindings during execution.");
    }
    if (translations.empty()) {
        throw std::runtime_error("Native module registration requires translations.");
    }
    for (const auto& entry : entries_) {
        if (entry.second.moduleId == moduleId) {
            throw std::runtime_error("Native module is already registered.");
        }
    }
    auto staged = entries_;
    for (const auto& translation : translations) {
        if (!translation.function || translation.expectedWords.empty() ||
            (translation.address & 3u) != 0 ||
            translation.expectedWords.size() >
                (std::numeric_limits<uint32_t>::max() -
                 static_cast<uint64_t>(translation.address) + 1u) / 4u) {
            throw std::runtime_error("Invalid native translation descriptor.");
        }
        if (translation.installationSource != 0 &&
            ((translation.installationSource & 3u) != 0 ||
             translation.expectedWords.size() >
                (std::numeric_limits<uint32_t>::max() -
                 static_cast<uint64_t>(translation.installationSource) + 1u) / 4u)) {
            throw std::runtime_error("Invalid native translation installation source.");
        }
        if (!translation.installationWords.empty() &&
            (translation.installationSource == 0 ||
             translation.installationWords.size() != translation.expectedWords.size())) {
            throw std::runtime_error("Invalid native patched-vector installation words.");
        }
        ppc_memory_offset(state, translation.address, translation.expectedWords.size() * 4u);
        if (translation.installationSource != 0) {
            auto source = translation;
            source.address = translation.installationSource;
            if (!translation.installationWords.empty()) source.expectedWords = translation.installationWords;
            CheckCode(state, source);
        } else {
            CheckCode(state, translation);
        }
        if (!staged.emplace(translation.address, Entry{moduleId, translation}).second) {
            throw std::runtime_error("Native function address is already registered: " +
                                     Address(translation.address) + ".");
        }
    }
    entries_.swap(staged);
}

void NativeDispatcher::RegisterRel(
    const RelImage& image, const LinkedRelModule& module, const PpcState& state,
    const std::vector<NativeRelTranslation>& translations) {
    if (image.id() == 0 || image.id() != module.id ||
        image.sections().size() != module.sectionAddresses.size()) {
        throw std::runtime_error("Native REL binding does not match the linked module.");
    }
    std::vector<NativeTranslation> bindings;
    for (const auto& binding : translations) {
        if (binding.section >= image.sections().size()) {
            throw std::runtime_error("Native REL binding has an invalid section.");
        }
        const auto& section = image.sections()[binding.section];
        const uint64_t address = static_cast<uint64_t>(module.sectionAddresses[binding.section]) +
                                 binding.offset;
        if (!section.executable || binding.offset > section.size ||
            binding.translation.expectedWords.size() > (section.size - binding.offset) / 4u ||
            address != binding.translation.address) {
            throw std::runtime_error("Native REL translation requires its compiled placement and executable range.");
        }
        bindings.push_back(binding.translation);
    }
    Register(module.id, state, bindings);
}

void NativeDispatcher::Unregister(uint32_t moduleId) {
    if (executing_) {
        throw std::runtime_error("Cannot change native bindings during execution.");
    }
    bool removed = false;
    for (auto entry = entries_.begin(); entry != entries_.end();) {
        if (entry->second.moduleId == moduleId) {
            entry = entries_.erase(entry);
            removed = true;
        } else {
            ++entry;
        }
    }
    if (!removed) {
        throw std::runtime_error("Native module is not registered.");
    }
}

bool NativeDispatcher::Contains(uint32_t address) const {
    return entries_.find(address) != entries_.end();
}

void NativeDispatcher::Run(PpcState& state, uint32_t address, uint32_t returnAddress,
                           size_t& remaining, size_t depth, size_t maxDepth) {
    if (depth > maxDepth) {
        throw std::runtime_error("Native guest call-depth limit exceeded.");
    }
    state.pc = address;
    do {
        ppc_check_pending_interrupts(state, state.externalInterruptsEnabled);
        while (state.externalInterruptsEnabled && ppc_si_interrupt_pending(state)) {
            ppc_deliver_external_interrupt(state);
        }
        if (remaining == 0) {
            throw std::runtime_error("Native function-entry budget exceeded at " + Address(state.pc) + ".");
        }
        const auto entry = entries_.find(state.pc);
        if (entry == entries_.end()) {
            throw std::runtime_error("No native translation registered at " + Address(state.pc) + ".");
        }
        CheckCode(state, entry->second.translation);
        --remaining;
        entry->second.translation.function(state);
    } while (state.pc != returnAddress);
}

void NativeDispatcher::Execute(PpcState& state, uint32_t address,
                               size_t maxFunctionEntries, size_t maxCallDepth) {
    if (executing_ || maxFunctionEntries == 0 || maxCallDepth == 0 ||
        (address & 3u) != 0 || (state.lr & 3u) != 0) {
        throw std::runtime_error("Invalid or reentrant native execution request.");
    }
    auto previous = std::move(state.callGuest);
    auto previousException = std::move(state.callException);
    executing_ = true;
    size_t depth = 1;
    size_t remaining = maxFunctionEntries;
    try {
        state.callException = [this, &state, &remaining, &depth, maxCallDepth](
                                  uint32_t target, uint32_t resume, PpcState& guest) {
            if (&guest != &state || (target & 3u) != 0 || (resume & 3u) != 0) {
                throw std::runtime_error("Invalid native guest exception context or address.");
            }
            ++depth;
            try {
                Run(guest, target, resume, remaining, depth, maxCallDepth);
            } catch (const PpcInterruptContextReturn&) {
                if (target != 0x500u || !guest.externalInterrupt ||
                    guest.externalInterrupt->active || guest.pc != resume) {
                    --depth;
                    throw std::runtime_error("Invalid native interrupt context-return signal.");
                }
            } catch (...) {
                --depth;
                throw;
            }
            --depth;
        };
        state.callGuest = [this, &state, &remaining, &depth, maxCallDepth](
                              uint32_t target, PpcState& guest) {
            if (&guest != &state || (target & 3u) != 0 || (guest.lr & 3u) != 0) {
                throw std::runtime_error("Invalid native guest call context or address.");
            }
            ++depth;
            try {
                Run(guest, target, guest.lr, remaining, depth, maxCallDepth);
            } catch (...) {
                --depth;
                throw;
            }
            --depth;
        };
        Run(state, address, state.lr, remaining, depth, maxCallDepth);
    } catch (...) {
        state.callGuest = std::move(previous);
        state.callException = std::move(previousException);
        executing_ = false;
        throw;
    }
    state.callGuest = std::move(previous);
    state.callException = std::move(previousException);
    executing_ = false;
}
