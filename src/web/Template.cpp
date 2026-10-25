#include "web/Template.h"

#include "models/Errors.h"
#include "web/Html.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace paysphere::web {

namespace {

std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

} // namespace

Template::Template(std::string text) : text_(std::move(text)) {}

Template Template::load(const std::string& path) {
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    if (!file) {
        throw PaySphereError("Cannot read web template: " + path);
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return Template(contents.str());
}

std::string Template::render(const Values& values) const {
    std::string out;
    out.reserve(text_.size() + 256);
    std::size_t position = 0;
    while (true) {
        const std::size_t open = text_.find("{{", position);
        if (open == std::string::npos) {
            out.append(text_, position, std::string::npos);
            return out;
        }
        out.append(text_, position, open - position);

        const bool raw = text_.compare(open, 3, "{{{") == 0;
        const std::string closing = raw ? "}}}" : "}}";
        const std::size_t nameStart = open + (raw ? 3 : 2);
        const std::size_t close = text_.find(closing, nameStart);
        if (close == std::string::npos) {
            throw PaySphereError("Unclosed placeholder in web template");
        }
        const std::string name = trim(text_.substr(nameStart, close - nameStart));
        const auto value = values.find(name);
        if (value == values.end()) {
            throw PaySphereError("Web template value missing: " + name);
        }
        out += raw ? value->second : escapeHtml(value->second);
        position = close + closing.size();
    }
}

} // namespace paysphere::web
