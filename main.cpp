#include <CLI/CLI.hpp>
#include <fmt/base.h>
#include <httplib.h>
#include <yaml-cpp/yaml.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <vector>

using namespace std;

// ---------------------------------------------------------------------------
// Signal handling
// ---------------------------------------------------------------------------
volatile sig_atomic_t gAbort = 0;

static void onSignal(int) {
  gAbort = 1;
}

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------
struct CFRChunk {
  string date;
  string title;
  optional<string> chapter;
  optional<string> subchapter;
  optional<string> part;
  optional<string> subpart;
  optional<string> section; // formatted as "part.section", e.g. "61.1"
  optional<string> appendix;
};

// ---------------------------------------------------------------------------
// YAML parsing
// ---------------------------------------------------------------------------
static optional<string> optStr(const YAML::Node& entry, const string& key) {
  if (entry[key] && entry[key].IsScalar() && !entry[key].Scalar().empty()) {
    return entry[key].as<string>();
  }
  return nullopt;
}

static list<CFRChunk> parseYaml(const string& path) {
  YAML::Node config = YAML::LoadFile(path);

  const string date = config["date"].as<string>();

  list<CFRChunk> calls;

  for (auto partIt = config.begin(); partIt != config.end(); ++partIt) {
    if (partIt->first.as<string>() == "date") {
      continue;
    }

    for (const auto& entry : partIt->second) {
      const string title = entry["title"].as<string>();
      const auto chapter = optStr(entry, "chapter");
      const auto subchapter = optStr(entry, "subchapter");
      const auto part = optStr(entry, "part");
      const auto subpart = optStr(entry, "subpart");

      if (entry["section"] && entry["section"].IsSequence()) {
        for (const auto& sec : entry["section"]) {
          const string section = part.value_or("") + "." + sec.as<string>();
          calls.push_back({date, title, chapter, subchapter, part, subpart, section});
        }
      }

      if (entry["appendix"] && entry["appendix"].IsSequence()) {
        for (const auto& app : entry["appendix"]) {
          calls.push_back(
              {date, title, chapter, subchapter, part, nullopt, nullopt, app.as<string>()});
        }
      }
    }
  }

  return calls;
}

// ---------------------------------------------------------------------------
// TOC extraction
// ---------------------------------------------------------------------------
struct TOCEntry {
  string id;
  string text;
  bool isSection; // false = part/appendix, true = section (§)
};

static list<TOCEntry> extractHeadings(const string& content) {
  list<TOCEntry> entries;

  // Matches: id="<id>"...> optional-whitespace <h1 or h4 ...> heading text
  const regex re(R"re(id="([^"]+)"[^>]*>\s*<h([14])[^>]*>([^\n<]+))re", regex::optimize);

  for (auto it = sregex_iterator(content.begin(), content.end(), re); it != sregex_iterator();
       ++it) {
    const auto& m = *it;
    string id = m[1].str();
    int level = stoi(m[2].str());
    string text = m[3].str();

    // trim surrounding whitespace
    auto ltrim = text.find_first_not_of(" \t\r\n");
    auto rtrim = text.find_last_not_of(" \t\r\n");
    if (ltrim == string::npos) {
      continue;
    }
    text = text.substr(ltrim, rtrim - ltrim + 1);

    entries.push_back({id, text, level == 4});
  }
  return entries;
}

// ---------------------------------------------------------------------------
// URL / filename helpers
// ---------------------------------------------------------------------------
static string buildPath(const CFRChunk& call) {
  return "/api/renderer/v1/content/enhanced/" + call.date + "/title-" + call.title;
}

static httplib::Params buildParams(const CFRChunk& call) {
  httplib::Params params;
  auto add = [&](const string& key, const optional<string>& val) {
    if (val) {
      params.emplace(key, *val);
    }
  };
  add("chapter", call.chapter);
  add("subchapter", call.subchapter);
  add("part", call.part);
  add("subpart", call.subpart);
  add("section", call.section);
  add("appendix", call.appendix);
  return params;
}

static string generateFilename(const CFRChunk& call) {
  string name = "title-" + call.title;
  if (call.chapter) {
    name += "-chapter-" + *call.chapter;
  }
  if (call.subchapter) {
    name += "-subchapter-" + *call.subchapter;
  }
  if (call.part) {
    name += "-part-" + *call.part;
  }
  if (call.subpart) {
    name += "-subpart-" + *call.subpart;
  }
  if (call.section) {
    name += "-section-" + *call.section;
  }
  if (call.appendix) {
    name += "-appendix-" + *call.appendix;
  }
  return name + ".html";
}

// ---------------------------------------------------------------------------
// HTML combine types
// ---------------------------------------------------------------------------
struct Fragment {
  CFRChunk call;
  string filename;
  string content;
};

struct PartGroup {
  string partKey;
  list<pair<string, string>> sections; // (anchor id, display text)
};

// ---------------------------------------------------------------------------
// Part title lookup via eCFR structure API
// ---------------------------------------------------------------------------
static map<string, string> fetchPartTitles(httplib::SSLClient& client, const string& date,
                                           const string& title) {
  map<string, string> titles;

  const string path = "/api/versioner/v1/structure/" + date + "/title-" + title + ".json";
  const auto res = client.Get(path);
  if (!res || res->status != 200) {
    fmt::println("Warning: could not fetch title structure (HTTP {})", res ? res->status : 0);
    return titles;
  }

  const YAML::Node root = YAML::Load(res->body);

  function<void(const YAML::Node&)> walk = [&](const YAML::Node& node) {
    if (!node.IsMap()) {
      return;
    }
    if (node["type"] && node["type"].as<string>() == "part") {
      titles[node["identifier"].as<string>()] = node["label_description"].as<string>();
    }
    if (node["children"] && node["children"].IsSequence()) {
      for (const auto& child : node["children"]) {
        walk(child);
      }
    }
  };

  walk(root);
  return titles;
}

// ---------------------------------------------------------------------------
// Temp directory
// ---------------------------------------------------------------------------
static string createTempDir() {
  const string tmpl = (filesystem::temp_directory_path() / "cfrd-XXXXXX").string();
  vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  if (!mkdtemp(buf.data())) {
    return {};
  }
  return buf.data();
}

// ---------------------------------------------------------------------------
// HTML writer
// ---------------------------------------------------------------------------
static void writeHtml(ofstream& out, const list<PartGroup>& partGroups,
                      const list<Fragment>& fragments, const map<string, string>& partTitles) {
  out << "<!DOCTYPE html>\n"
      << "<html lang=\"en\">\n"
      << "<head>\n"
      << "  <meta charset=\"UTF-8\">\n"
      << "  <title>14 CFR</title>\n"
      << "  <style>\n"
      << "    nav#toc { margin: 2em 0; }\n"
      << "    nav#toc ul { list-style: none; padding: 0; }\n"
      << "    .toc-part    { font-weight: bold; margin-top: 0.75em; }\n"
      << "    .toc-section { margin-left: 2em; font-weight: normal; }\n"
      << "  </style>\n"
      << "</head>\n"
      << "<body>\n"
      << "<nav id=\"toc\">\n"
      << "  <h2>Table of Contents</h2>\n"
      << "  <ul>\n";

  for (const auto& group : partGroups) {
    string partLabel = "Part " + group.partKey;
    auto it = partTitles.find(group.partKey);
    if (it != partTitles.end()) {
      partLabel += ": " + it->second;
    }
    out << "    <li class=\"toc-part\">"
        << "<a href=\"#part-" << group.partKey << "\">" << partLabel << "</a>"
        << "</li>\n";
    for (const auto& [id, text] : group.sections) {
      out << "    <li class=\"toc-section\">"
          << "<a href=\"#" << id << "\">" << text << "</a>"
          << "</li>\n";
    }
  }

  out << "  </ul>\n"
      << "</nav>\n"
      << "<hr>\n";

  string currentPart;
  for (const auto& frag : fragments) {
    const string partKey = frag.call.part.value_or("unknown");
    if (partKey != currentPart) {
      if (!currentPart.empty()) {
        out << "</div>\n";
      }
      out << "<div id=\"part-" << partKey << "\">\n";
      currentPart = partKey;
    }
    out << frag.content << "\n";
  }
  if (!currentPart.empty()) {
    out << "</div>\n";
  }

  out << "</body>\n</html>\n";
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
  // Install signal handlers for clean abort on Ctrl+C / SIGTERM
  signal(SIGINT, onSignal);
  signal(SIGTERM, onSignal);

  // Parse CLI arguments
  CLI::App app{"FAA CFR downloader"};
  app.set_version_flag("--version", "0.1.0");

  string yamlPath;
  string outputDir = filesystem::current_path().string();
  bool testMode = false;

  app.add_option("parts", yamlPath, "Parts YAML file (e.g. cfr-parts.yaml)")->required();
  app.add_option("-o,--output", outputDir, "Output directory (default: cwd)");
  app.add_flag("-t,--test", testMode, "Limit to first 5 calls for testing");

  CLI11_PARSE(app, argc, argv);

  // Parse the YAML config into a list of CFRChunk API calls
  list<CFRChunk> calls = parseYaml(yamlPath);
  list<string> errors;

  if (testMode) {
    calls.resize(5);
    fmt::println("Test mode: limiting to first 5 calls");
  }

  const size_t total = calls.size();
  fmt::println("Loaded {} API calls", total);

  // Create a temp directory to stage downloaded fragments
  const string tmpDir = createTempDir();
  if (tmpDir.empty()) {
    fmt::println("Failed to create temp directory.");
    return 1;
  }
  fmt::println("Staging downloads in {}", tmpDir);

  // Set up the HTTPS client
  httplib::SSLClient client("www.ecfr.gov");
  client.set_follow_location(true);
  client.set_connection_timeout(30);
  client.set_read_timeout(60);

  // Fetch part titles from the eCFR structure API for TOC headings
  const string date = calls.front().date;
  const string title = calls.front().title;
  fmt::println("Fetching title {} structure...", title);
  const auto partTitles = fetchPartTitles(client, date, title);

  // Download each CFR chunk sequentially, writing fragments to temp dir
  size_t completed = 0;
  list<pair<CFRChunk, string>> downloaded; // (call, filename) in download order

  for (const auto& call : calls) {
    if (gAbort) {
      fmt::println("\nAborted after {}/{} calls.", completed, total);
      break;
    }

    const auto res = client.Get(buildPath(call), buildParams(call), httplib::Headers{});

    if (!res || res->status != 200) {
      const string err =
          buildPath(call) + (res ? " HTTP " + to_string(res->status) : " (no response)");
      errors.push_back(err);
      fmt::println("  ✗ {}", err);
      continue;
    }

    const string filename = generateFilename(call);
    ofstream ofs(tmpDir + "/" + filename);
    if (!ofs) {
      errors.push_back("Failed to write: " + filename);
      fmt::println("  ✗ Failed to write: {}", filename);
      continue;
    }
    ofs << res->body;
    downloaded.emplace_back(call, filename);
    fmt::println("  ✓ {}", filename);
    ++completed;

    this_thread::sleep_for(chrono::milliseconds(500));
  }

  // Combine fragments into a single HTML file with TOC
  if (!gAbort && !downloaded.empty()) {
    filesystem::create_directories(outputDir);

    // Buffer all fragment content from temp dir
    list<Fragment> fragments;
    for (const auto& [call, filename] : downloaded) {
      ifstream in(tmpDir + "/" + filename);
      fragments.push_back(
          {call, filename, string(istreambuf_iterator<char>(in), istreambuf_iterator<char>())});
    }

    // Group fragments by part to drive TOC structure
    list<PartGroup> partGroups;
    for (const auto& frag : fragments) {
      const string partKey = frag.call.part.value_or("unknown");
      if (partGroups.empty() || partGroups.back().partKey != partKey) {
        partGroups.push_back({partKey, {}});
      }
      for (const auto& entry : extractHeadings(frag.content)) {
        partGroups.back().sections.emplace_back(entry.id, entry.text);
      }
    }

    // Write combined HTML then clean up temp dir
    const string outFile =
        (filesystem::path(outputDir) / filesystem::path(yamlPath).stem()).string() + ".html";
    ofstream out(outFile);
    writeHtml(out, partGroups, fragments, partTitles);

    filesystem::remove_all(tmpDir);
    fmt::println("\n{}/{} calls completed. Combined HTML: {}", completed, total, outFile);
  } else if (gAbort) {
    fmt::println("Temp files retained in: {}", tmpDir);
  }

  // Report any errors
  if (!errors.empty()) {
    fmt::println("\n{} error(s):", errors.size());
    for (const auto& err : errors) {
      fmt::println("  ✗ {}", err);
    }
  }

  return errors.empty() ? 0 : 1;
}
