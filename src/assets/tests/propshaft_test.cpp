// Unit tests of the Propshaft code (Rails: propshaft 1.2.1; Rust: crates/assets/build/propshaft.rs).
#include <doctest.h>

#include <filesystem>
#include <fstream>

#include "assets/importmap.hpp"
#include "assets/propshaft.hpp"
#include "assets/regex.hpp"

namespace fs = std::filesystem;
namespace build = campfire::assets::build;

namespace {

struct Dir {
  fs::path path;
  Dir() : path(fs::temp_directory_path() / ("cfassets-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)))) {
    fs::create_directories(path);
  }
  ~Dir() { std::error_code ec; fs::remove_all(path, ec); }
  Dir(const Dir&) = delete;
  Dir& operator=(const Dir&) = delete;
  void write(const std::string& name, const std::string& content) const {
    fs::create_directories((path / name).parent_path());
    std::ofstream(path / name, std::ios::binary) << content;
  }
};

}  // namespace

TEST_CASE("extname follows File.extname") {
  CHECK(build::extname("a/b.css") == ".css");
  CHECK(build::extname(".hidden") == "");
  CHECK(build::extname("a.b/c") == "");
  CHECK(build::extname("x.min.js") == ".js");
}

TEST_CASE("a CSS url() is rewritten to the digested path") {
  Dir dir;
  dir.write("images/a.png", "png");
  dir.write("app.css", "body { background: url(images/a.png?x=1) }\n.b { background: url(\"data:image/png;base64,AA\") }\n");
  auto lp = build::LoadPath::create({dir.path}, "1.0", "/assets");
  REQUIRE(lp.has_value());
  const auto css = lp->find("app.css");
  REQUIRE(css.has_value());
  const auto compiled = lp->compiled_content(*css);
  REQUIRE(compiled.has_value());
  REQUIRE(compiled->has_value());
  const std::string image = lp->digested_path(*lp->find("images/a.png"));
  CHECK(compiled->value() ==
        "body { background: url(\"/assets/" + image + "?x=1\") }\n.b { background: url(\"data:image/png;base64,AA\") }\n");
  // The digest of the stylesheet covers the files that it references.
  const std::string before = lp->digested_path(*css);
  dir.write("images/a.png", "other");
  auto changed = build::LoadPath::create({dir.path}, "1.0", "/assets");
  REQUIRE(changed.has_value());
  CHECK(changed->digested_path(*changed->find("app.css")) != before);
}

TEST_CASE("the first directory wins for a logical path, and a digested name stays") {
  Dir first;
  Dir second;
  first.write("a.js", "one");
  second.write("a.js", "two");
  second.write("b-12345678.digested.js", "x");
  auto lp = build::LoadPath::create({first.path, second.path}, "1", "/assets");
  REQUIRE(lp.has_value());
  CHECK(lp->assets()[*lp->find("a.js")].content == "one");
  CHECK(lp->digested_path(*lp->find("b-12345678.digested.js")) == "b-12345678.digested.js");
  CHECK(lp->digested_path(*lp->find("a.js")).starts_with("a-"));
}

TEST_CASE("the import map expands pins and directories") {
  Dir root;
  root.write("config/importmap.rb",
             "pin \"application\"\n# a comment\npin \"@x/y\", to: \"x.js\" # note\n"
             "pin_all_from \"app/javascript/controllers\", under: \"controllers\", preload: false\n");
  root.write("app/javascript/controllers/index.js", "");
  root.write("app/javascript/controllers/a_controller.js", "");
  root.write("app/javascript/controllers/sub/index.js", "");
  const auto pins = build::expand_importmap(root.path / "config/importmap.rb", root.path);
  REQUIRE(pins.has_value());
  REQUIRE(pins->size() == 5);
  CHECK((*pins)[0].name == "application");
  CHECK((*pins)[0].path == "application.js");
  CHECK((*pins)[1].path == "x.js");
  CHECK((*pins)[2].name == "controllers/a_controller");
  CHECK((*pins)[3].name == "controllers");
  CHECK((*pins)[3].path == "controllers/index.js");
  CHECK((*pins)[4].name == "controllers/sub");
  CHECK(!(*pins)[3].preload);
}
