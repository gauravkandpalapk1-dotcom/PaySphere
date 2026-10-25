#pragma once

#include <map>
#include <string>

namespace paysphere::web {

// A minimal HTML template: the page is written as a normal .html file under
// web/templates/, with placeholders filled in at render time.
//
//   {{name}}    the value, HTML-escaped (use for anything a user typed)
//   {{{name}}}  the value inserted as-is (only for HTML this program built
//               itself, already escaped)
//
// Rendering with a missing value throws, so a typo in a template is caught
// immediately instead of showing a blank.
class Template {
public:
    using Values = std::map<std::string, std::string>;

    explicit Template(std::string text);
    // Reads a template file; throws PaySphereError if it cannot be read.
    static Template load(const std::string& path);

    std::string render(const Values& values) const;

private:
    std::string text_;
};

} // namespace paysphere::web
