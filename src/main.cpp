#include "cfr_types.h"
#include "config.hpp"
#include "ecfr_xsl.h"

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
#include <set>
#include <sstream>
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

static string buildXmlPath(const CFRChunk& call) {
  return "/api/versioner/v1/full/" + call.date + "/title-" + call.title + ".xml";
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
  return name;
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
// Link rewriting
// ---------------------------------------------------------------------------

// Collect every id="..." attribute value from all fragments.
static set<string> collectIds(const list<Fragment>& fragments)
{
  set<string> ids;
  const regex re(R"re(id="([^"]+)")re", regex::optimize);
  for (const auto& frag : fragments)
    for (auto it = sregex_iterator(frag.content.begin(), frag.content.end(), re);
         it != sregex_iterator(); ++it)
      ids.insert((*it)[1].str());
  return ids;
}

// Rewrite eCFR-relative hrefs in a fragment:
//   /on/DATE/title-N/...  and  /current/...
// If the resolved anchor target exists in docIds  → local #anchor
// Otherwise                                       → full https://www.ecfr.gov URL
static string rewriteLinks(const string& content, const set<string>& docIds)
{
  static const string base = "https://www.ecfr.gov";

  // Matches href="/on/..." or href="/current/..."
  const regex re(R"re(href="(/(on|current)/[^"]*)")re", regex::optimize);

  string result;
  result.reserve(content.size());

  auto it  = sregex_iterator(content.begin(), content.end(), re);
  auto end = sregex_iterator();
  size_t pos = 0;

  for (; it != end; ++it) {
    const auto& m    = *it;
    const string url = m[1].str();   // e.g. /on/2026-06-01/title-14/part-61/section-61.56

    result.append(content, pos, m.position() - pos);

    // Extract the fragment anchor if present (e.g. #p-61.56(a))
    string localId;
    const auto hashPos = url.find('#');
    if (hashPos != string::npos) {
      localId = url.substr(hashPos + 1);
    } else {
      // Derive id from the last path component
      // /on/DATE/title-N/part-P/section-P.S  → P.S
      // /on/DATE/title-N/part-P              → part-P
      // /on/DATE/title-N/part-P/appendix-X  → Appendix-X-to-Part-P (eCFR uses this id format)
      const auto lastSlash = url.rfind('/');
      if (lastSlash != string::npos) {
        string seg = url.substr(lastSlash + 1);
        // Remove query string
        const auto q = seg.find('?');
        if (q != string::npos) seg = seg.substr(0, q);

        if (seg.substr(0, 8) == "section-")
          localId = seg.substr(8);   // "section-61.56" → "61.56"
        else if (seg.substr(0, 5) == "part-")
          localId = seg;             // "part-61" → "part-61"
        else
          localId = seg;
      }
    }

    if (!localId.empty() && docIds.count(localId))
      result += "href=\"#" + localId + "\"";
    else
      result += "href=\"" + base + url + "\"";

    pos = m.position() + m.length();
  }
  result.append(content, pos, string::npos);
  return result;
}

// ---------------------------------------------------------------------------
// XML writer
// ---------------------------------------------------------------------------
// Escape special XML characters in plain text before embedding in XML.
static string xmlEscape(const string& s) {
  string r;
  r.reserve(s.size());
  for (char c : s) {
    if      (c == '&') r += "&amp;";
    else if (c == '<') r += "&lt;";
    else if (c == '>') r += "&gt;";
    else               r += c;
  }
  return r;
}

static string buildXmlString(const list<Fragment>& fragments,
                               const map<string, string>& partTitles = {}) {
  ostringstream oss;
  oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<cfr>\n";

  // Embed part title metadata so the XSL can display descriptions in the TOC.
  if (!partTitles.empty()) {
    oss << "<cfr-meta>\n";
    for (const auto& [partN, desc] : partTitles)
      oss << "  <cfr-part n=\"" << partN << "\">" << xmlEscape(desc) << "</cfr-part>\n";
    oss << "</cfr-meta>\n";
  }

  for (const auto& frag : fragments) {
    string content = frag.content;
    // Strip per-fragment XML declaration so the combined document stays valid
    if (content.size() >= 5 && content.substr(0, 5) == "<?xml") {
      const auto end = content.find("?>");
      if (end != string::npos) {
        content = content.substr(end + 2);
        const auto start = content.find_first_not_of(" \t\r\n");
        if (start != string::npos) content = content.substr(start);
      }
    }
    oss << content << "\n";
  }
  oss << "</cfr>\n";
  return oss.str();
}

static void writeXml(ofstream& out, const list<Fragment>& fragments) {
  out << buildXmlString(fragments);
}

// ---------------------------------------------------------------------------
// HTML writer
// ---------------------------------------------------------------------------
static void writeHtml(ofstream& out, const list<PartGroup>& partGroups,
                      const list<Fragment>& fragments, const map<string, string>& partTitles) {
  const set<string> docIds = collectIds(fragments);
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
    out << rewriteLinks(frag.content, docIds) << "\n";
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
  app.set_version_flag("--version", CFRD_VERSION);

  string yamlPath;
  string outputDir = filesystem::current_path().string();
  string cssPath;
  bool testMode   = false;
  bool xmlMode    = false;
  bool styledMode = false;
  bool cacheMode  = false;

  app.add_option("parts", yamlPath, "Parts YAML file (e.g. cfr-parts.yaml)")->required();
  app.add_option("-o,--output", outputDir, "Output directory (default: cwd)");
  app.add_flag("-t,--test", testMode, "Limit to first 5 calls for testing");
  app.add_flag("--xml", xmlMode, "Output raw XML instead of HTML");
  app.add_flag("--styled", styledMode,
               "Apply ecfr.xsl and embed CSS for styled HTML output");
  app.add_option("--css", cssPath,
                 "CSS file to embed instead of the built-in default (requires --styled)");
  app.add_flag("--cache", cacheMode,
               "Persist downloaded fragment files and reuse them on subsequent runs "
               "(cache stored in cfrd-cache/<yaml-stem>/ beside the YAML file)");

  CLI11_PARSE(app, argc, argv);

  if (styledMode && xmlMode) {
    fmt::println("Warning: --styled and --xml are mutually exclusive; --xml takes precedence.");
    styledMode = false;
  }
  if (!cssPath.empty() && !styledMode) {
    fmt::println("Warning: --css has no effect without --styled, ignoring.");
  }

  // Parse the YAML config into a list of CFRChunk API calls
  list<CFRChunk> calls = parseYaml(yamlPath);
  list<string> errors;

  if (testMode) {
    calls.resize(5);
    fmt::println("Test mode: limiting to first 5 calls");
  }

  const size_t total   = calls.size();
  const string yamlStem = filesystem::path(yamlPath).stem().string();
  fmt::println("Loaded {} API calls", total);

  // Set up the working directory for downloaded fragment files.
  // --cache: persistent directory beside the YAML, reused across runs.
  // default: random /tmp directory, deleted on successful completion.
  const string cacheDir = (filesystem::path(yamlPath).parent_path()
                            / "cfrd-cache" / yamlStem).string();
  string workDir;
  if (cacheMode) {
    workDir = cacheDir;
    filesystem::create_directories(workDir);
    fmt::println("Cache directory: {}", workDir);
  } else {
    workDir = createTempDir();
    if (workDir.empty()) {
      fmt::println("Failed to create temp directory.");
      return 1;
    }
    fmt::println("Staging downloads in {}", workDir);
  }

  // Set up the HTTPS client
  httplib::SSLClient client("www.ecfr.gov");
  client.set_follow_location(true);
  client.set_connection_timeout(30);
  client.set_read_timeout(60);

  // Fetch part titles from the eCFR structure API for TOC headings.
  // Used by both the HTML renderer and the styled XSL path; skipped for
  // raw XML output where no TOC is generated.
  const string date = calls.front().date;
  const string title = calls.front().title;
  map<string, string> partTitles;
  if (!xmlMode) {
    fmt::println("Fetching title {} structure...", title);
    partTitles = fetchPartTitles(client, date, title);
  }

  // Download each CFR chunk sequentially, writing fragments to the work dir.
  // With --cache, files that already exist on disk are used as-is.
  size_t completed   = 0;
  size_t cachedCount = 0;
  list<pair<CFRChunk, string>> downloaded; // (call, filename) in download order

  for (const auto& call : calls) {
    if (gAbort) {
      fmt::println("\nAborted after {}/{} calls.", completed, total);
      break;
    }

    const bool useXmlApi = xmlMode || styledMode;
    const string filename = generateFilename(call) + (useXmlApi ? ".xml" : ".html");
    const string filepath = workDir + "/" + filename;

    // Cache hit: reuse existing file, no network call needed
    if (cacheMode && filesystem::exists(filepath)) {
      downloaded.emplace_back(call, filename);
      fmt::println("  ↺ {} (cached)", filename);
      ++completed;
      ++cachedCount;
      continue;
    }

    const string reqPath = useXmlApi ? buildXmlPath(call) : buildPath(call);
    const httplib::Params params = buildParams(call);

    // Retry up to 3 times on transient server errors (503, 429).
    // Backoff: 2 s, 4 s, 8 s between attempts.
    httplib::Result res{nullptr, httplib::Error::Success};
    for (int attempt = 0; attempt < 3; ++attempt) {
      res = client.Get(reqPath, params, httplib::Headers{});
      if (res && (res->status == 503 || res->status == 429)) {
        const int waitSec = 2 << attempt; // 2, 4, 8
        fmt::println("  ↻ HTTP {} — retrying in {}s (attempt {}/3)",
                     res->status, waitSec, attempt + 1);
        this_thread::sleep_for(chrono::seconds(waitSec));
        continue;
      }
      break;
    }

    if (!res || res->status != 200) {
      const string err =
          reqPath + (res ? " HTTP " + to_string(res->status) : " (no response)");
      errors.push_back(err);
      fmt::println("  ✗ {}", err);
      continue;
    }

    ofstream ofs(filepath);
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

    // Buffer all fragment content from the work directory
    list<Fragment> fragments;
    for (const auto& [call, filename] : downloaded) {
      ifstream in(workDir + "/" + filename);
      fragments.push_back(
          {call, filename, string(istreambuf_iterator<char>(in), istreambuf_iterator<char>())});
    }

    const string ext     = xmlMode ? ".xml" : ".html";
    const string outFile = (filesystem::path(outputDir) / yamlStem).string() + ext;

    if (xmlMode) {
      ofstream out(outFile);
      writeXml(out, fragments);
    } else if (styledMode) {
      // Load CSS: user-supplied file or built-in default
      string css(kEcfrDefaultCss);
      if (!cssPath.empty()) {
        ifstream cssFile(cssPath);
        if (!cssFile) {
          fmt::println("Warning: cannot read --css file '{}', using default.", cssPath);
        } else {
          css = string(istreambuf_iterator<char>(cssFile), {});
        }
      }
      const string xmlContent = buildXmlString(fragments, partTitles);
      applyEcfrXslFromString(xmlContent, css, outFile);
    } else {
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
      ofstream out(outFile);
      writeHtml(out, partGroups, fragments, partTitles);
    }

    if (!cacheMode) {
      filesystem::remove_all(workDir);
    }

    if (cachedCount > 0) {
      fmt::println("\n{}/{} calls completed ({} cached, {} downloaded). Output: {}",
                   completed, total, cachedCount, completed - cachedCount, outFile);
    } else {
      fmt::println("\n{}/{} calls completed. Output: {}", completed, total, outFile);
    }
  } else if (gAbort) {
    if (!cacheMode) {
      fmt::println("Temp files retained in: {}", workDir);
    }
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
