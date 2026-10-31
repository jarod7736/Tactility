#include "agents/Card.h"

#include <cJSON.h>
#include <cstring>
#include <memory>

namespace agents {

namespace {

struct JsonDeleter {
    void operator()(cJSON* json) const { cJSON_Delete(json); }
};

std::string stringField(const cJSON* object, const char* name) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : "";
}

bool boolField(const cJSON* object, const char* name) {
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(object, name));
}

bool parseKind(const char* name, CardKind& kind) {
    static constexpr struct { const char* name; CardKind kind; } KINDS[] = {
        { "user", CardKind::User }, { "step", CardKind::Step }, { "text", CardKind::Text },
        { "approval", CardKind::Approval }, { "question", CardKind::Question },
        { "done", CardKind::Done }, { "error", CardKind::Error },
    };
    for (const auto& entry : KINDS) {
        if (strcmp(entry.name, name) == 0) {
            kind = entry.kind;
            return true;
        }
    }
    return false;
}

} // namespace

std::optional<Card> parseCard(std::string_view json) {
    std::unique_ptr<cJSON, JsonDeleter> root(cJSON_ParseWithLength(json.data(), json.size()));
    if (!root || !cJSON_IsObject(root.get())) {
        return std::nullopt;
    }

    const cJSON* seq = cJSON_GetObjectItemCaseSensitive(root.get(), "seq");
    const cJSON* kindItem = cJSON_GetObjectItemCaseSensitive(root.get(), "kind");
    if (!cJSON_IsNumber(seq) || !cJSON_IsString(kindItem) || kindItem->valuestring == nullptr) {
        return std::nullopt;
    }

    Card card;
    if (!parseKind(kindItem->valuestring, card.kind)) {
        return std::nullopt;
    }
    card.seq = seq->valueint;
    card.title = stringField(root.get(), "title");
    card.body = stringField(root.get(), "body");
    card.truncated = boolField(root.get(), "truncated");
    card.resolved = boolField(root.get(), "resolved");

    const cJSON* ts = cJSON_GetObjectItemCaseSensitive(root.get(), "ts");
    if (cJSON_IsNumber(ts)) {
        card.ts = static_cast<int64_t>(ts->valuedouble);
    }

    const cJSON* options = cJSON_GetObjectItemCaseSensitive(root.get(), "options");
    if (cJSON_IsArray(options)) {
        const cJSON* option = nullptr;
        cJSON_ArrayForEach(option, options) {
            if (!cJSON_IsObject(option)) continue;
            std::string key = stringField(option, "key");
            if (key.size() != 1) continue;
            card.options.push_back(CardOption { key[0], stringField(option, "label") });
        }
    }
    return card;
}

} // namespace agents
