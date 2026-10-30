// Codex CLI (codex exec --json). No sample has been captured yet, so every line falls back to
// the generic parser. Replace the body with the real mapping once a sample is in tests/fixtures/codex.

#include "llm_stream_internal.h"

namespace pc {
namespace llm {

bool ParseCodexLines(const nlohmann::json&, const std::string&, const std::string&, std::vector<LlmStreamEvent>&) {
    return false;
}

}  // namespace llm
}  // namespace pc
