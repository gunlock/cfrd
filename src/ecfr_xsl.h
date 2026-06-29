#pragma once
#include <libxml/tree.h>
#include <string>
#include <string_view>

// ---------------------------------------------------------------------------
// ecfr.xsl — transforms the <cfr> wrapper produced by cfrd's XML download
// into semantic HTML.  Every eCFR element maps to an HTML element plus a
// stable CSS class so that either the built-in stylesheet or a user-supplied
// one can control presentation.
//
// CSS injection marker: the XSL emits  <style>/* INJECT_CSS */</style>  and
// applyEcfrXsl() replaces that marker with the chosen CSS before writing the
// output file.
//
// Class contract (for CSS authors):
//
//   .cfr-body             <body>
//   .cfr-part             DIV5  — regulatory part
//   .cfr-subpart          DIV6  — subpart
//   .cfr-subjgrp          DIV7  — subject group / reserved
//   .cfr-section          DIV8  — section
//   .cfr-appendix         DIV9  — appendix / SFAR
//   .cfr-heading          HEAD  — section/part heading (h2–h4 by depth)
//   .cfr-heading-2/3      HD2, HD3
//   .cfr-hed              HED   — inline sub-heading
//   .cfr-para             P     — paragraph
//   .cfr-para-item        P starting with '(' — base class for all item levels
//   .cfr-item-l1          (a)(b)(c)  — single lowercase, 2em indent
//   .cfr-item-l2          (1)(2)(3)  — digit, 4em indent
//   .cfr-item-l3          (ii)(iii)  — roman numeral, 6em indent
//   .cfr-item-l4          (A)(B)(C)  — uppercase, 8em indent
//   .cfr-para-cont        P2    — continuation paragraph
//   .cfr-flush-para       FP    — flush paragraph
//   .cfr-flush-dash       FP-DASH
//   .cfr-table            TABLE
//   .cfr-authority        AUTH
//   .cfr-source           SOURCE
//   .cfr-citation         CITA
//   .cfr-section-authority SECAUTH
//   .cfr-note             NOTE
//   .cfr-editorial-note   EDNOTE
//   .cfr-extract          EXTRACT
//   .cfr-approval         APPRO
//   .cfr-pspace           PSPACE
//   .cfr-fr-cite          FR
//   .cfr-effective-date   EFFDNOT
//   .cfr-xref             XREF
// ---------------------------------------------------------------------------

inline constexpr std::string_view kEcfrXsl = R"ECFRXSL(<?xml version="1.0" encoding="UTF-8"?>
<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="html" encoding="UTF-8" indent="yes"/>

  <!-- Key: group sections by part number extracted from the section number.
       The versioner API returns bare DIV8/DIV9 elements for section-level
       requests — no DIV5/DIV6 wrappers — so part number is derived from
       the section number itself (e.g. "61.1" → part "61"). -->
  <xsl:key name="sections-by-part" match="DIV8" use="substring-before(@N,'.')"/>

  <!-- ================================================================
       Document root — the <cfr> wrapper written by cfrd writeXml().
       Generates TOC then renders content.
       ================================================================ -->
  <xsl:template match="cfr">
    <html lang="en">
      <head>
        <meta charset="UTF-8"/>
        <title>14 CFR</title>
        <style>/* INJECT_CSS */</style>
      </head>
      <body class="cfr-body">

        <!-- Table of Contents: group sections by part number (digits before
             the first dot in the section number, e.g. "61" from "61.1").
             Part descriptions come from cfr-meta injected by cfrd.
             Appendices are listed under their part via "Part N" in @N. -->
        <nav id="toc" class="cfr-toc">
          <h2 class="cfr-toc-heading">Table of Contents</h2>
          <ul class="cfr-toc-list">
            <xsl:for-each select="//DIV8[generate-id()=generate-id(
                                    key('sections-by-part',
                                        substring-before(@N,'.'))[1])]">
              <xsl:sort select="number(substring-before(@N,'.'))" data-type="number"/>
              <xsl:variable name="pn"   select="substring-before(@N,'.')"/>
              <xsl:variable name="desc" select="/cfr/cfr-meta/cfr-part[@n=$pn]"/>
              <li class="cfr-toc-part">
                <a href="#part-{$pn}">
                  <xsl:text>Part </xsl:text><xsl:value-of select="$pn"/>
                  <xsl:if test="$desc">
                    <xsl:text> — </xsl:text>
                    <xsl:value-of select="$desc"/>
                  </xsl:if>
                </a>
                <ul class="cfr-toc-sections">
                  <xsl:for-each select="//DIV8[substring-before(@N,'.')=$pn]">
                    <li class="cfr-toc-section">
                      <a href="#{@N}"><xsl:value-of select="HEAD"/></a>
                    </li>
                  </xsl:for-each>
                  <xsl:for-each select="//DIV9[substring-after(@N,'Part ')=$pn]">
                    <li class="cfr-toc-section">
                      <a href="#{translate(@N,' ','-')}">
                        <xsl:value-of select="HEAD"/>
                      </a>
                    </li>
                  </xsl:for-each>
                </ul>
              </li>
            </xsl:for-each>
          </ul>
        </nav>
        <hr class="cfr-toc-rule"/>

        <xsl:apply-templates/>
      </body>
    </html>
  </xsl:template>

  <!-- Suppress the metadata block injected by cfrd — display only. -->
  <xsl:template match="cfr-meta"/>

  <!-- ================================================================
       eCFR API wrapper elements — pass through, no output.
       DIV1–DIV7 don't appear in section-level downloads but are listed
       for safety in case of full-title or future API changes.
       ================================================================ -->
  <xsl:template match="ECFR|VOLUME|DIV1|DIV2|DIV3|DIV4|DIV5|DIV6|DIV7">
    <xsl:apply-templates/>
  </xsl:template>

  <!-- ================================================================
       Structural divisions
       ================================================================ -->

  <!-- Section (DIV8).
       Before the first section of each part, inject a cfr-part anchor div
       so TOC links (#part-N) have a target in the document. -->
  <xsl:template match="DIV8">
    <xsl:variable name="pn" select="substring-before(@N,'.')"/>
    <xsl:if test="generate-id()=generate-id(key('sections-by-part',$pn)[1])">
      <xsl:variable name="desc" select="/cfr/cfr-meta/cfr-part[@n=$pn]"/>
      <div class="cfr-part" id="part-{$pn}">
        <h2 class="cfr-heading">
          <xsl:text>Part </xsl:text><xsl:value-of select="$pn"/>
          <xsl:if test="$desc">
            <xsl:text> — </xsl:text><xsl:value-of select="$desc"/>
          </xsl:if>
        </h2>
      </div>
    </xsl:if>
    <div class="cfr-section" id="{@N}">
      <xsl:apply-templates/>
    </div>
  </xsl:template>

  <!-- Appendix / SFAR (DIV9) -->
  <xsl:template match="DIV9">
    <div class="cfr-appendix">
      <xsl:if test="@N">
        <xsl:attribute name="id">
          <xsl:value-of select="translate(@N,' ','-')"/>
        </xsl:attribute>
      </xsl:if>
      <xsl:apply-templates/>
    </div>
  </xsl:template>

  <!-- ================================================================
       Headings — section/appendix heads are always h4; anything outside
       a section (unlikely in practice) falls back to h3.
       ================================================================ -->
  <xsl:template match="HEAD">
    <xsl:choose>
      <xsl:when test="ancestor::DIV8 or ancestor::DIV9">
        <h4 class="cfr-heading"><xsl:apply-templates/></h4>
      </xsl:when>
      <xsl:otherwise>
        <h3 class="cfr-heading"><xsl:apply-templates/></h3>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <xsl:template match="HED">
    <strong class="cfr-hed"><xsl:apply-templates/></strong>
  </xsl:template>
  <xsl:template match="HD2">
    <h4 class="cfr-heading cfr-heading-2"><xsl:apply-templates/></h4>
  </xsl:template>
  <xsl:template match="HD3">
    <h5 class="cfr-heading cfr-heading-3"><xsl:apply-templates/></h5>
  </xsl:template>

  <!-- ================================================================
       Paragraphs
       ================================================================ -->

  <!-- Paragraphs starting with '(' are lettered/numbered items.
       Detect the level from the character(s) between the first '(' and ')':
         single lowercase  → (a)(b)(c)…   level 1
         digit             → (1)(2)(3)…   level 2
         2+ lowercase      → (ii)(iii)…   level 3  (roman numerals)
         uppercase         → (A)(B)(C)…   level 4
       cfr-para-item is retained on all levels so custom CSS can target
       the whole group; cfr-item-l1…l4 give per-level control. -->
  <xsl:template match="P">
    <xsl:variable name="t"     select="normalize-space(.)"/>
    <xsl:variable name="inner" select="substring-before(substring-after($t,'('),')')"/>
    <xsl:variable name="fc"    select="substring($inner,1,1)"/>
    <xsl:choose>
      <!-- not an item paragraph -->
      <xsl:when test="not(starts-with($t,'('))">
        <p class="cfr-para"><xsl:apply-templates/></p>
      </xsl:when>
      <!-- digit → (1)(2)(3) level 2 -->
      <xsl:when test="string-length(translate($fc,'0123456789',''))=0">
        <p class="cfr-para cfr-para-item cfr-item-l2"><xsl:apply-templates/></p>
      </xsl:when>
      <!-- uppercase → (A)(B)(C) level 4 -->
      <xsl:when test="string-length(translate($fc,'ABCDEFGHIJKLMNOPQRSTUVWXYZ',''))=0">
        <p class="cfr-para cfr-para-item cfr-item-l4"><xsl:apply-templates/></p>
      </xsl:when>
      <!-- 2+ lowercase chars → (ii)(iii)(iv) roman numerals level 3 -->
      <xsl:when test="string-length($inner) &gt; 1">
        <p class="cfr-para cfr-para-item cfr-item-l3"><xsl:apply-templates/></p>
      </xsl:when>
      <!-- single lowercase → (a)(b)(c) level 1 -->
      <xsl:otherwise>
        <p class="cfr-para cfr-para-item cfr-item-l1"><xsl:apply-templates/></p>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <xsl:template match="P2">
    <p class="cfr-para cfr-para-cont"><xsl:apply-templates/></p>
  </xsl:template>
  <xsl:template match="FP">
    <p class="cfr-flush-para"><xsl:apply-templates/></p>
  </xsl:template>
  <!-- FP-DASH / FP-1 / FP-2: hyphen/digit after hyphen makes these
       ambiguous with XPath arithmetic; match via local-name() predicate. -->
  <xsl:template match="*[local-name()='FP-DASH']">
    <p class="cfr-flush-para cfr-flush-dash"><xsl:apply-templates/></p>
  </xsl:template>
  <xsl:template match="*[local-name()='FP-1']">
    <p class="cfr-flush-para cfr-flush-1"><xsl:apply-templates/></p>
  </xsl:template>
  <xsl:template match="*[local-name()='FP-2']">
    <p class="cfr-flush-para cfr-flush-2"><xsl:apply-templates/></p>
  </xsl:template>

  <!-- ================================================================
       Inline formatting
       ================================================================ -->
  <xsl:template match="I">
    <em><xsl:apply-templates/></em>
  </xsl:template>

  <!-- E with T="52" is superscript; all others are emphasis.
       Known T values in Title 14: 03=email/bold, 04=italic variant,
       51=small-caps, 52=superscript, 54=subscript variant, 9145=unknown.
       Safe fallback for unrecognised values is <em>. -->
  <xsl:template match="E[@T='52']">
    <sup><xsl:apply-templates/></sup>
  </xsl:template>
  <xsl:template match="E">
    <em><xsl:apply-templates/></em>
  </xsl:template>

  <!-- Native superscript / subscript elements (distinct from E[@T='52']) -->
  <xsl:template match="sup"><sup><xsl:apply-templates/></sup></xsl:template>
  <xsl:template match="sub"><sub><xsl:apply-templates/></sub></xsl:template>

  <!-- Line break -->
  <xsl:template match="br"><br/></xsl:template>

  <!-- ================================================================
       Tables — already HTML-structured; lowercase the tag names
       ================================================================ -->
  <xsl:template match="TABLE">
    <table class="cfr-table">
      <xsl:copy-of select="@border|@cellpadding|@cellspacing|@width|@frame"/>
      <xsl:apply-templates/>
    </table>
  </xsl:template>

  <xsl:template match="THEAD"><thead><xsl:apply-templates/></thead></xsl:template>
  <xsl:template match="TBODY"><tbody><xsl:apply-templates/></tbody></xsl:template>
  <xsl:template match="TFOOT"><tfoot><xsl:apply-templates/></tfoot></xsl:template>
  <xsl:template match="CAPTION"><caption><xsl:apply-templates/></caption></xsl:template>

  <xsl:template match="TR">
    <tr><xsl:copy-of select="@*"/><xsl:apply-templates/></tr>
  </xsl:template>
  <xsl:template match="TH">
    <th><xsl:copy-of select="@*"/><xsl:apply-templates/></th>
  </xsl:template>
  <xsl:template match="TD">
    <td><xsl:copy-of select="@*"/><xsl:apply-templates/></td>
  </xsl:template>

  <!-- Generic DIV (e.g. table container gpotbl_div) — unwrap -->
  <xsl:template match="DIV">
    <xsl:apply-templates/>
  </xsl:template>

  <!-- ================================================================
       Metadata and annotation blocks
       ================================================================ -->
  <xsl:template match="AUTH">
    <div class="cfr-authority"><xsl:apply-templates/></div>
  </xsl:template>
  <xsl:template match="SOURCE">
    <div class="cfr-source"><xsl:apply-templates/></div>
  </xsl:template>
  <!-- CITA TYPE="N" — docket/amendment history. Suppressed from display. -->
  <xsl:template match="CITA"/>
  <xsl:template match="SECAUTH">
    <div class="cfr-section-authority"><xsl:apply-templates/></div>
  </xsl:template>
  <xsl:template match="NOTE">
    <div class="cfr-note"><xsl:apply-templates/></div>
  </xsl:template>
  <!-- EDNOTE — editorial notes referencing the CFR Sections Affected list.
       Not useful in a downloaded reference document; suppress. -->
  <xsl:template match="EDNOTE"/>
  <xsl:template match="EXTRACT">
    <blockquote class="cfr-extract"><xsl:apply-templates/></blockquote>
  </xsl:template>
  <xsl:template match="APPRO">
    <p class="cfr-approval"><xsl:apply-templates/></p>
  </xsl:template>
  <xsl:template match="PSPACE">
    <span class="cfr-pspace"><xsl:apply-templates/></span>
  </xsl:template>
  <xsl:template match="FR">
    <span class="cfr-fr-cite"><xsl:apply-templates/></span>
  </xsl:template>
  <xsl:template match="EFFDNOT">
    <div class="cfr-effective-date"><xsl:apply-templates/></div>
  </xsl:template>
  <!-- XREF — pending-amendment notices ("Link to an amendment published at…").
       These have no valid target in a downloaded document, so suppress them. -->
  <xsl:template match="XREF"/>

  <!-- ================================================================
       Suppress pure-metadata nodes with no display value
       ================================================================ -->
  <xsl:template match="AMDDATE"/>

</xsl:stylesheet>
)ECFRXSL";

// ---------------------------------------------------------------------------
// Default CSS — embedded so output HTML is fully self-contained.
// Pass --css <file> to substitute your own.
// ---------------------------------------------------------------------------
inline constexpr std::string_view kEcfrDefaultCss = R"ECFRCSS(
/* cfrd default stylesheet — targets classes emitted by ecfr.xsl.
   Pass --css <your-file.css> on the command line to override. */

*, *::before, *::after { box-sizing: border-box; }

body.cfr-body {
  font-family: Georgia, 'Times New Roman', serif;
  font-size: 16px;
  line-height: 1.6;
  color: #1a1a1a;
  max-width: 860px;
  margin: 2rem auto;
  padding: 0 1.5rem;
}

/* ----- Headings ----- */
.cfr-heading {
  font-family: 'Helvetica Neue', Arial, sans-serif;
  color: #002868;
  margin: 1.5em 0 0.4em;
  line-height: 1.3;
}
.cfr-part    > .cfr-heading { font-size: 1.5rem; border-bottom: 2px solid #002868; padding-bottom: 0.3em; }
.cfr-subpart > .cfr-heading { font-size: 1.2rem; border-bottom: 1px solid #ccc;    padding-bottom: 0.2em; }
.cfr-section  > .cfr-heading { font-size: 1rem; font-weight: bold; }
.cfr-appendix > .cfr-heading { font-size: 1.1rem; font-style: italic; }
.cfr-heading-2, .cfr-heading-3 { font-size: 0.95rem; }

/* ----- Table of Contents ----- */
.cfr-toc { margin: 1.5rem 0 2rem; }
.cfr-toc-heading {
  font-family: 'Helvetica Neue', Arial, sans-serif;
  color: #002868;
  font-size: 1.2rem;
  margin-bottom: 0.75em;
}
.cfr-toc-list  { list-style: none; padding: 0; margin: 0; }
.cfr-toc-sections { list-style: none; padding: 0; margin: 0; }
.cfr-toc-part  {
  font-family: 'Helvetica Neue', Arial, sans-serif;
  font-weight: bold;
  margin-top: 0.75em;
}
.cfr-toc-part  > a:hover { text-decoration: underline; }
.cfr-toc-section { margin-left: 1.5em; font-weight: normal; font-size: 0.9em; }
.cfr-toc-section > a:hover { text-decoration: underline; }
.cfr-toc-rule { border: none; border-top: 2px solid #002868; margin: 1.5rem 0; }

/* ----- Structural blocks ----- */
.cfr-part     { margin-bottom: 3rem; }
.cfr-subpart  { margin: 2rem 0; }

/* One blank line above every section — the bottom margin produces the gap
   between the previous section's last line and this one's heading. */
.cfr-section  {
  margin: 1.6em 0 0 0;
}
.cfr-appendix { margin: 2rem 0; padding: 1rem 1.25rem; background: #fafafa;
                border: 1px solid #ddd; border-radius: 4px; }
.cfr-subjgrp  { margin: 1rem 0; padding-left: 0.5rem; }

/* ----- Paragraphs ----- */
.cfr-para { margin: 0.3em 0; }

/* Item paragraphs: no blank lines between consecutive (a)/(1)/(i) items.
   Each level is indented 2 em per step with a hanging indent for the label. */
.cfr-para-item { margin: 0; }

/* padding-left sets the indent per level; text-indent hangs the label
   left so wrapped lines align with the text content after the label. */
.cfr-item-l1 { padding-left: 2em;  text-indent: -1.5em; }
.cfr-item-l2 { padding-left: 4em;  text-indent: -1.5em; }
.cfr-item-l3 { padding-left: 6em;  text-indent: -1.5em; }
.cfr-item-l4 { padding-left: 8em;  text-indent: -1.5em; }

.cfr-para-cont  { margin-top: 0; }
.cfr-flush-para { margin: 0.25em 0; }
.cfr-flush-dash { margin: 0.25em 0; }
.cfr-flush-1    { margin: 0; padding-left: 2em; }
.cfr-flush-2    { margin: 0; padding-left: 4em; }

/* ----- Tables ----- */
.cfr-table {
  border-collapse: collapse;
  width: 100%;
  margin: 1em 0;
  font-size: 0.9em;
  font-family: 'Helvetica Neue', Arial, sans-serif;
}
.cfr-table caption {
  font-style: italic;
  margin-bottom: 0.5em;
  text-align: left;
  font-size: 0.95em;
}
.cfr-table th,
.cfr-table td {
  border: 1px solid #aaa;
  padding: 0.35em 0.6em;
  vertical-align: top;
  text-align: left;
}
.cfr-table thead th             { background: #002868; color: #fff; }
.cfr-table tbody tr:nth-child(even) { background: #f5f7fb; }

/* ----- Metadata and annotations ----- */
.cfr-authority,
.cfr-source {
  font-size: 0.8em;
  color: #666;
  font-style: italic;
  margin-top: 1em;
  padding-top: 0.5em;
  border-top: 1px solid #eee;
}
.cfr-citation { font-size: 0.75em; color: #888; margin-top: 0.5em; }
.cfr-section-authority,
.cfr-approval { font-size: 0.8em; color: #666; margin-top: 0.5em; }

/* ----- Notes and callouts ----- */
.cfr-note {
  margin: 1em 0;
  padding: 0.75em 1em;
  border-left: 4px solid #f0a500;
  background: #fffbf0;
  font-size: 0.9em;
}
.cfr-editorial-note {
  font-size: 0.8em;
  color: #777;
  font-style: italic;
  margin: 0.5em 0;
}
.cfr-extract {
  margin: 1em 2em;
  padding: 0.5em 1em;
  border-left: 3px solid #ccc;
  color: #333;
}
.cfr-effective-date { font-size: 0.85em; color: #666; }

/* ----- Inline ----- */
.cfr-body em { font-style: italic; font-weight: bold; }
.cfr-xref    { color: #0044cc; }
.cfr-fr-cite { font-size: 0.85em; color: #555; }
.cfr-pspace  { white-space: pre; }
.cfr-hed     { display: block; font-weight: bold; margin-bottom: 0.25em; }
)ECFRCSS";

// ---------------------------------------------------------------------------
// Apply the embedded ecfr.xsl to `inputDoc`, inject `cssContent` in place of
// the /* INJECT_CSS */ marker, and write the result to `outPath`.
// Caller is responsible for xmlInitParser() / xsltCleanupGlobals() lifecycle.
// ---------------------------------------------------------------------------
bool applyEcfrXsl(xmlDocPtr inputDoc, const std::string& cssContent,
                  const std::string& outPath);

// ---------------------------------------------------------------------------
// Convenience overload: parse `xmlContent` (the <cfr>…</cfr> string produced
// by cfrd), apply ecfr.xsl, inject CSS, write to `outPath`.
// Handles xmlInitParser / xsltCleanupGlobals internally.
// ---------------------------------------------------------------------------
bool applyEcfrXslFromString(const std::string& xmlContent,
                             const std::string& cssContent,
                             const std::string& outPath);
