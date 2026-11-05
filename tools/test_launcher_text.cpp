#include "launcher_text.hpp"
#include "update_text.hpp"
#include <cstdio>
#include <cstdlib>

using namespace launcher::text;

namespace {
void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "%s\n", what); std::exit(1); }
}
}

// The launcher edits the player's live ERVR.ini while the game hot-reloads it: every byte it does not mean
// to change must survive, and it must read keys exactly as GetPrivateProfileString does.
int main() {
    const std::string ini =
        "\xEF\xBB\xBF; ERVR\r\n[General]\r\nLogLevel=info\r\n\r\n[VR]\r\n; comment = not a key\r\n"
        "StereoMode = full\r\nRenderScale=1.0\r\n; trailing comment\r\n\r\n[HUD]\r\nMode=quad\r\n[vr]\r\nWorldScale=9\r\n";
    Check(iniGet(ini, "General", "LogLevel") == "info", "plain key");
    Check(iniGet(ini, "vr", "stereomode") == "full", "section and key match without case, value trimmed");
    Check(!iniGet(ini, "VR", "comment").has_value(), "comment lines are no keys");
    Check(!iniGet(ini, "VR", "WorldScale").has_value(), "only the first section of a name counts");
    Check(!iniGet(ini, "Combat", "Enabled").has_value(), "missing section");

    std::string edited = iniSet(ini, "VR", "StereoMode", "cinema");
    Check(iniGet(edited, "VR", "StereoMode") == "cinema", "value replaced");
    Check(edited.find("StereoMode = cinema\r\n") != std::string::npos, "key spelling and spacing kept");
    Check(edited.size() == ini.size() + 2, "nothing else changes");
    Check(edited.substr(0, 3) == "\xEF\xBB\xBF", "byte order mark kept");

    edited = iniSet(ini, "VR", "WorldScale", "0.8");
    Check(edited.find("RenderScale=1.0\r\nWorldScale=0.8\r\n; trailing comment") != std::string::npos,
          "a missing key goes after its section's last key, with the file's line ends");
    Check(iniGet(edited, "VR", "WorldScale") == "0.8", "inserted key reads back");

    edited = iniSet(ini, "Combat", "Enabled", "false");
    Check(iniGet(edited, "Combat", "Enabled") == "false", "missing section appended");
    Check(edited.find("WorldScale=9\r\n\r\n[Combat]\r\nEnabled=false\r\n") != std::string::npos, "appended at the end");

    Check(iniGet(iniSet("[A]\nx=1", "A", "y", "2"), "A", "y") == "2", "last line without a line end");

    // Numbers the Options tab writes: whole numbers keep their zeros (0.6.5 wrote 90 as 9, 200 as 2).
    Check(iniNumber(90, 0) == "90" && iniNumber(200, 0) == "200" && iniNumber(100, 0) == "100", "whole numbers");
    Check(iniNumber(0, 0) == "0" && iniNumber(45, 0) == "45", "zero and plain whole numbers");
    Check(iniNumber(1.f, 2) == "1.0" && iniNumber(0.86f, 2) == "0.86" && iniNumber(0.5f, 2) == "0.5", "decimals");
    Check(iniNumber(2.5f, 2) == "2.5" && iniNumber(10.f, 2) == "10.0", "trailing zeros after the point only");
    Check(iniSet("[A]\nx=1", "A", "y", "2") == "[A]\nx=1\ny=2\n", "the new key starts its own line");
    Check(iniSet("[A]\nx=\n", "A", "x", "3") == "[A]\nx=3\n", "empty value");

    // The game's GraphicsConfig.xml, in its own shape: every other setting the player chose must survive.
    const std::wstring graphics =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\" ?>\r\n<config><ScreenMode>WINDOW</ScreenMode>\r\n"
        L"<Auto-detectBestRenderingSettings>ON</Auto-detectBestRenderingSettings>\r\n<QualitySetting>MAX</QualitySetting>\r\n"
        L"<SSAO>MAX</SSAO>\r\n<DepthOfField>MAX</DepthOfField>\r\n<MotionBlur>DISABLE</MotionBlur>\r\n"
        L"<RaytracingQuality> HIGH </RaytracingQuality>\r\n</config>\r\n";
    Check(xmlGet(graphics, "DepthOfField") == "MAX" && xmlGet(graphics, "RaytracingQuality") == "HIGH", "xml value, trimmed");
    Check(!xmlGet(graphics, "Depth").has_value(), "a tag matches whole");
    std::wstring xml = graphics;
    std::vector<std::string> changed = turnEffectsOff(xml, allEffects());
    Check(changed == std::vector<std::string>{"depth of field (was MAX)", "ray tracing (was HIGH)"},
          "what changed; motion blur was off already");
    Check(xmlGet(xml, "DepthOfField") == "DISABLE" && xmlGet(xml, "MotionBlur") == "DISABLE" &&
              xmlGet(xml, "RaytracingQuality") == "DISABLE",
          "all three off");
    Check(xmlGet(xml, "QualitySetting") == "CUSTOM" && xmlGet(xml, "Auto-detectBestRenderingSettings") == "OFF",
          "preset and auto-detect can't put them back");
    Check(xml.find(L"<SSAO>MAX</SSAO>\r\n<DepthOfField>DISABLE</DepthOfField>") != std::wstring::npos &&
              xml.rfind(L"</config>\r\n") == xml.size() - 11,
          "nothing else changes");
    Check(turnEffectsOff(xml, allEffects()).empty(), "a second pass changes nothing");
    xml = graphics;
    Check(turnEffectsOff(xml, EffectsOff{false, true, false}).empty() && xml == graphics,
          "an effect left to the game stays, and nothing else moves");
    xml = L"<config><DepthOfField>LOW</DepthOfField></config>";
    Check(turnEffectsOff(xml, allEffects()).size() == 1 && xml == L"<config><DepthOfField>DISABLE</DepthOfField></config>",
          "missing tags stay missing");
    Check(listed({"a"}) == "a" && listed({"a", "b"}) == "a and b" && listed({"a", "b", "c"}) == "a, b and c", "lists");

    // ERVR.log's lines.
    LogLine line = parseLogLine("17:36:02:114 [ 1234] | INFO  | ERVR 0.4.0 starting (pid 99) in C:\\Game\\ERVR");
    Check(line.time == "17:36:02" && line.kind == LineKind::good, "start line is a milestone");
    Check(line.text.rfind("ERVR 0.4.0 starting", 0) == 0, "text after the level");
    Check(parseLogLine("17:36:02:114 [ 1234] | WARN  | [xr] x").kind == LineKind::warning, "warning");
    Check(parseLogLine("17:36:02:114 [ 1234] | ERROR | boom").kind == LineKind::error, "error");
    Check(parseLogLine("17:36:02:114 [ 1234] | DEBUG | hb").kind == LineKind::muted, "debug is muted");
    Check(parseLogLine("17:36:02:114 [ 1234] | INFO  | ERVR.ini reloaded").kind == LineKind::normal, "plain info");
    Check(parseLogLine("Starting Steam...").time.empty(), "launcher lines stay whole");

    Progress progress;
    Check(noteProgress("ERVR 0.4.0 starting (pid 99) in X", progress) && progress.loaded && progress.version == "0.4.0",
          "loaded");
    noteProgress("[xr] xrCreateInstance failed: XR_ERROR_RUNTIME_FAILURE (-2). The runtime ...", progress);
    Check(progress.waiting, "waiting for the runtime");
    noteProgress("[xr] runtime \"VirtualDesktopXR\" 1.0.9", progress);
    Check(!progress.waiting && progress.runtime == "VirtualDesktopXR", "runtime");
    noteProgress("[xr] system \"Meta Quest 3\": recommended eye 2688x2880, max 4096x4096", progress);
    Check(progress.headset == "Meta Quest 3" && progress.eye == "2688x2880", "headset and eye");
    noteProgress("[xr] session running", progress);
    Check(progress.vr, "in VR");

    // Steam.
    const std::string vdf = "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"D:\\\\SteamLibrary\"\n\t}\n}";
    Check(vdfValues(vdf, "PATH") == std::vector<std::string>{"D:\\SteamLibrary"}, "vdf path, escapes decoded");

    // Versions and the shared address-map list.
    Check(parseVersion("2.7.2.0") == std::array<int, 4>{2, 7, 2, 0}, "version");
    Check(parseVersion("v0.4") == std::array<int, 4>{0, 4, 0, 0}, "short version");
    Check(verifiedPatch({2, 7, 2, 0}) == "1.17.2" && verifiedPatch({2, 6, 2, 0}) == "1.16.2", "verified builds");
    Check(verifiedPatch({2, 7, 3, 0}).empty(), "unknown build");

    // Runtimes.
    Check(runtimeLabel("C:\\Program Files\\Oculus\\Support\\oculus-runtime\\oculus_openxr_64.json") == "Meta Quest Link",
          "oculus");
    Check(jsonStrings("{\"runtime\" : [ \"C:\\\\Steam\\\\SteamVR\" ], \"config\": [\"x\"]}", "runtime") ==
              std::vector<std::string>{"C:\\Steam\\SteamVR"},
          "openvrpaths runtime list");
    Check(jsonStrings("{\"runtime\": {\"name\": \"Oculus\", \"library_path\": \"x\"}}", "name") ==
              std::vector<std::string>{"Oculus"},
          "manifest name");

    const ProbeResult probe = parseProbe("runtime: Oculus 1.106.0\r\nheadset: Meta Quest 3\neye: 2064x2208\n");
    Check(probe.runtime == "Oculus 1.106.0" && probe.headset == "Meta Quest 3" && probe.eye == "2064x2208", "probe");

    Check(quoteArgument(L"C:\\Program Files\\x\\") == L"\"C:\\Program Files\\x\\\\\"", "quoting");
    Check(withoutUserName("C:\\Users\\Jane Doe\\AppData\\x") == "~\\AppData\\x", "home folder written as ~");
    ReportFacts facts;
    facts.version = "0.4.0";
    facts.lastError = "line\nwith ``` fence";
    const std::string report = reportSummary(facts);
    Check(report.rfind("```\nERVR 0.4.0 report\n", 0) == 0 && report.find("with ''' fence") != std::string::npos,
          "report keeps to one fence");

    // ---- Updates: GitHub's answer for v0.7.1 (shape as returned 2026-11-05, trimmed) ----
    const std::string latest =
        R"({"html_url":"https://github.com/IlyaMez/EldenringVR/releases/tag/v0.7.1","tag_name":"v0.7.1",)"
        R"("assets":[{"name":"ERVR-v0.7.1.zip","size":6615558,)"
        R"("digest":"sha256:13D0D4A5A5218E1BE5847A6E46C1BDB349FACCEDF4A3CEE9655A0C5B783636EA",)"
        R"("browser_download_url":"https://github.com/IlyaMez/EldenringVR/releases/download/v0.7.1/ERVR-v0.7.1.zip"},)"
        R"({"name":"ERVR-v0.7.1.zip.sha256","size":83,"digest":"sha256:6406b194f4aae68023ba3db9891c5614f2bbae195655dc617b2cc68302c3e469",)"
        R"("browser_download_url":"https://github.com/IlyaMez/EldenringVR/releases/download/v0.7.1/ERVR-v0.7.1.zip.sha256"}],)"
        R"("body":"## Install\r\n```\r\nSHA256  13d0d4a5a5218e1be5847a6e46c1bdb349faccedf4a3cee9655a0c5b783636ea\r\n```\r\n é"})";
    std::string why;
    auto release = parseRelease(latest, why);
    Check(release && release->version == "0.7.1" && release->zipName == "ERVR-v0.7.1.zip" &&
              release->zipSize == 6615558 && release->zipUrl.rfind("https://", 0) == 0,
          "the release, its zip and its size");
    Check(release && release->sha256 == "13d0d4a5a5218e1be5847a6e46c1bdb349faccedf4a3cee9655a0c5b783636ea",
          "the zip's SHA-256 from GitHub's digest, lower case");
    std::string noDigest = latest;
    noDigest.replace(noDigest.find("\"digest\":\"sha256:13D0"), 83, "\"digest\":null,");
    release = parseRelease(noDigest, why);
    Check(release && release->sha256 == "13d0d4a5a5218e1be5847a6e46c1bdb349faccedf4a3cee9655a0c5b783636ea",
          "without a digest, the SHA256 line of the release notes");
    std::string noHash = noDigest;
    noHash.replace(noHash.find("SHA256  13d0"), 6, "sha-25");
    Check(!parseRelease(noHash, why) && why.find("SHA-256") != std::string::npos, "no SHA-256 at all: refused");
    std::string otherAsset = latest;
    otherAsset.replace(otherAsset.find("\"name\":\"ERVR-v0.7.1.zip\""), 24, "\"name\":\"Source-0.7.1.zip\"");
    Check(!parseRelease(otherAsset, why) && why.find("ERVR-v0.7.1.zip") != std::string::npos, "no player zip: refused");
    std::string plainHttp = latest;
    plainHttp.replace(plainHttp.find("https://github.com/IlyaMez/EldenringVR/releases/download/v0.7.1/ERVR-v0.7.1.zip\""), 8,
                      "http://");
    Check(!parseRelease(plainHttp, why), "a download that is not https: refused");
    Check(!parseRelease(R"({"tag_name":"nightly","assets":[]})", why) && why.find("nightly") != std::string::npos,
          "a tag that is no version: refused");
    Check(!parseRelease("<html>rate limited</html>", why), "an answer that is not JSON: refused");
    Check(newerVersion("0.7.2", "0.7.1") && newerVersion("0.10.0", "0.9.9") && !newerVersion("0.7.1", "0.7.1") &&
              !newerVersion("0.6.5", "0.7.1"),
          "version order");
    Check(releaseVersion("0.7.1") && !releaseVersion("0.7") && !releaseVersion("0.7.1.0") && !releaseVersion("0.7.x"),
          "release versions are three numbers");

    // What an update does to the launcher's folder: the zip's files replace and add; an older ERVR's program file
    // no longer shipped in ERVR\ goes; settings, logs and files beside the launcher stay.
    const std::vector<std::wstring> installed = {L"ERVR Launcher.exe", L"dinput8.dll", L"ReShade.ini", L"my notes.txt",
                                                 L"ERVR/ERVR.dll", L"ERVR/ERVR.ini", L"ERVR/old_helper.dll",
                                                 L"ERVR/ERVR.log", L"ERVR/notes.ini", L"reshade-shaders/x.fx"};
    const std::vector<std::wstring> package = {L"ERVR Launcher.exe", L"dinput8.dll", L"dxgi.dll", L"ReShade.ini",
                                               L"ERVR/ERVR.dll", L"ERVR/ERVR.ini", L"ERVR/README.md"};
    const UpdatePlan plan = updatePlan(installed, package);
    const auto has = [](const std::vector<std::wstring>& list, const wchar_t* path) {
        return std::find(list.begin(), list.end(), path) != list.end();
    };
    Check(plan.replace.size() == 5 && has(plan.replace, L"ERVR/ERVR.dll") && has(plan.replace, L"ERVR Launcher.exe"),
          "shipped files that exist are replaced");
    Check(plan.add.size() == 2 && has(plan.add, L"dxgi.dll") && has(plan.add, L"ERVR/README.md"), "new files are added");
    Check(plan.retire.size() == 1 && has(plan.retire, L"ERVR/old_helper.dll"),
          "only an older ERVR's program file goes; logs, settings and the player's files stay");
}
