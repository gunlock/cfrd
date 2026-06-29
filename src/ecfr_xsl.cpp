#include "ecfr_xsl.h"

#include <libxml/parser.h>
#include <libxslt/transform.h>
#include <libxslt/xslt.h>
#include <libxslt/xsltutils.h>

#include <fmt/base.h>

#include <fstream>
#include <regex>
#include <set>

using namespace std;

// ---------------------------------------------------------------------------
// Link post-processing
// ---------------------------------------------------------------------------

// Collect every id="..." value from the serialised HTML so we know which
// section anchors are local to this document.
static set<string> collectHtmlIds(const string& html) {
  set<string> ids;
  const regex re(R"re(id="([^"]+)")re", regex::optimize);
  for (sregex_iterator it(html.begin(), html.end(), re); it != sregex_iterator(); ++it)
    ids.insert((*it)[1].str());
  return ids;
}

// Rewrite plain-text § X.Y cross-references in HTML text nodes to hyperlinks.
//   Local sections (id exists in docIds) → <a href="#X.Y">
//   External sections                    → <a href="https://www.ecfr.gov/current/title-N/section-X.Y">
//
// Only text between HTML tags is processed; attribute values are untouched.
static string linkifyHtml(const string& html, const string& title) {
  const set<string> docIds = collectHtmlIds(html);
  const string secBase  = "https://www.ecfr.gov/current/title-" + title + "/section-";
  const string partBase = "https://www.ecfr.gov/current/title-" + title + "/part-";

  // Three alternatives:
  //   Group 1: full § or §§ section ref   "§ 61.1"  "§§ 5.21"
  //   Group 2: section number              "61.1"
  //   Group 3: full part ref               "Part 61" / "parts 91"
  //   Group 4: part label prefix           "Part " / "parts "
  //   Group 5: part number                 "61"
  //   Group 6: list separator              ", "  "; "  ", and "
  //   Group 7: bare section num after sep  "5.23"  (the §§ X, Y, Z pattern)
  //
  // (?:§§|§) matches both the single and plural section symbols, with §§
  // listed first so the greedy alternation consumes both characters.
  // \b before [Pp] prevents matching mid-word (e.g. "counterpart 61").
  const regex linkRe(
      "((?:§§|§)\\s+(\\d+\\.\\d+\\w*))|(\\b([Pp]arts?\\s+)(\\d+))"
      "|([,;]\\s+(?:and\\s+)?)(\\d+\\.\\d+\\w*)",
      regex::optimize);

  string result;
  result.reserve(static_cast<size_t>(html.size() * 1.05));

  // Tokenise into HTML tags and text runs; only process text runs.
  const regex tokenRe("(<[^>]*>)|([^<]+)", regex::optimize);

  // Track anchor nesting so we never create <a> inside an existing <a>.
  int anchorDepth = 0;

  for (sregex_iterator it(html.begin(), html.end(), tokenRe);
       it != sregex_iterator(); ++it) {
    const auto& m = *it;
    if (m[1].matched) {
      const string tag = m[1].str();
      const size_t n   = tag.size();
      // Opening <a …> — second char 'a'/'A', third char space or '>'
      if (n >= 2 && (tag[1]=='a'||tag[1]=='A') && (n==2||tag[2]=='>'||tag[2]==' '))
        ++anchorDepth;
      // Closing </a>
      else if (n >= 4 && tag[1]=='/' && (tag[2]=='a'||tag[2]=='A') && tag[3]=='>')
        anchorDepth = anchorDepth > 0 ? anchorDepth - 1 : 0;
      result += tag;
      continue;
    }

    // Inside an existing anchor — output text unchanged, no re-linking.
    if (anchorDepth > 0) {
      result += m[2].str();
      continue;
    }

    const string text = m[2].str();
    size_t pos = 0;
    for (sregex_iterator si(text.begin(), text.end(), linkRe);
         si != sregex_iterator(); ++si) {
      const auto& sm = *si;
      result += text.substr(pos, sm.position() - pos);

      if (sm[2].matched) {
        // § X.Y or §§ X.Y — section reference (sm[0] preserves § or §§)
        const string secNum = sm[2].str();
        const string href = docIds.count(secNum) ? "#" + secNum : secBase + secNum;
        result += "<a href=\"" + href + "\">" + sm[0].str() + "</a>";
      } else if (sm[3].matched) {
        // [Pp]art(s?) N — part reference
        const string partNum = sm[5].str();
        const string partId  = "part-" + partNum;
        const string href = docIds.count(partId) ? "#" + partId : partBase + partNum;
        result += "<a href=\"" + href + "\">" + sm[0].str() + "</a>";
      } else {
        // Bare section number after a list separator (, or ;).
        // Emit the separator unchanged, then link the section number.
        result += sm[6].str();
        const string secNum = sm[7].str();
        const string href = docIds.count(secNum) ? "#" + secNum : secBase + secNum;
        result += "<a href=\"" + href + "\">" + secNum + "</a>";
      }

      pos = sm.position() + sm.length();
    }
    result += text.substr(pos);
  }

  return result;
}

// ---------------------------------------------------------------------------
// XSL transformation
// ---------------------------------------------------------------------------

bool applyEcfrXsl(xmlDocPtr inputDoc, const string& cssContent,
                  const string& outPath) {
  xmlDocPtr xslDoc =
      xmlReadMemory(kEcfrXsl.data(), (int)kEcfrXsl.size(), "ecfr.xsl",
                    nullptr,
                    XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
  if (!xslDoc) {
    fmt::println("Error: failed to parse embedded ecfr.xsl");
    return false;
  }

  xsltStylesheetPtr sheet = xsltParseStylesheetDoc(xslDoc); // takes ownership
  if (!sheet) {
    fmt::println("Error: failed to compile embedded ecfr.xsl");
    return false;
  }

  const char* params[] = {nullptr};
  xmlDocPtr result = xsltApplyStylesheet(sheet, inputDoc, params);
  if (!result) {
    xsltFreeStylesheet(sheet);
    fmt::println("Error: ecfr.xsl transformation failed");
    return false;
  }

  xmlChar* buf = nullptr;
  int len = 0;
  xsltSaveResultToString(&buf, &len, result, sheet);
  xmlFreeDoc(result);
  xsltFreeStylesheet(sheet); // also frees xslDoc

  if (!buf) {
    fmt::println("Error: failed to serialize transformed output");
    return false;
  }

  string html(reinterpret_cast<char*>(buf), len);
  xmlFree(buf);

  // Inject CSS
  const string marker = "/* INJECT_CSS */";
  const auto cssPos = html.find(marker);
  if (cssPos != string::npos)
    html.replace(cssPos, marker.size(), cssContent);

  // Linkify § X.Y cross-references in text nodes
  html = linkifyHtml(html, "14");

  ofstream out(outPath);
  if (!out) {
    fmt::println("Error: cannot open output file: {}", outPath);
    return false;
  }
  out << html;
  return true;
}

static void silenceXml(void*, const char*, ...) {}

bool applyEcfrXslFromString(const string& xmlContent, const string& cssContent,
                             const string& outPath) {
  xmlInitParser();
  xmlSetGenericErrorFunc(nullptr, silenceXml);
  xsltSetGenericErrorFunc(nullptr, silenceXml);

  xmlDocPtr doc =
      xmlReadMemory(xmlContent.c_str(), (int)xmlContent.size(), "ecfr.xml",
                    nullptr,
                    XML_PARSE_NONET | XML_PARSE_RECOVER |
                        XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
  if (!doc) {
    fmt::println("Error: failed to parse combined eCFR XML");
    xsltCleanupGlobals();
    xmlCleanupParser();
    return false;
  }

  const bool ok = applyEcfrXsl(doc, cssContent, outPath);

  xmlFreeDoc(doc);
  xsltCleanupGlobals();
  xmlCleanupParser();
  return ok;
}
