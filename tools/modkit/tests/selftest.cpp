// SPDX-License-Identifier: GPL-2.0-or-later
// Checks of the mod kit on made-up data (no game files needed).
#include "../src/csv.hpp"
#include "../src/patch.hpp"
#include "../src/xml.hpp"

#include <cstdio>
#include <cstdlib>

using namespace dcmod;

static int failures = 0;
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FAILED line %d: %s\n", __LINE__, #c);      \
            ++failures;                                             \
        }                                                           \
    } while (0)

static Bytes B(const std::string& s) { return Bytes(s.begin(), s.end()); }
static std::string S(const Bytes& b) { return std::string(b.begin(), b.end()); }

static std::string Run(MemorySource& src, const std::string& path, const std::string& patches) {
    PatchReport report;
    Workspace ws(src, report);
    PatchEnv env{ws, "t", "."};
    ApplyPatches(Json::Parse(patches), env);
    return S(ws.Finish().at(path));
}

static bool Fails(MemorySource& src, const std::string& patches) {
    try {
        Run(src, "x", patches);
    } catch (const Error&) {
        return true;
    }
    return false;
}

int main() {
    // CSV: only the changed cell differs, CRLF and quoting are kept
    {
        MemorySource src;
        src.files["t.csv"] = B("\"Name\",\"Speed\",\"Note\"\r\n\"a\",\"1\",\"x, y\"\r\n\"b\",\"2\",\"\"\r\n");
        const std::string out = Run(src, "t.csv", R"([{"op":"csv.set","file":"t.csv","match":"b","set":{"Speed":250}}])");
        CHECK(out == "\"Name\",\"Speed\",\"Note\"\r\n\"a\",\"1\",\"x, y\"\r\n\"b\",\"250\",\"\"\r\n");
        const std::string added = Run(src, "t.csv", R"([{"op":"csv.add","file":"t.csv","like":"a","row":{"Name":"c","Note":"q\"r"}}])");
        CHECK(added.find("\"c\",\"1\",\"q\"\"r\"\r\n") != std::string::npos);
        CHECK(Fails(src, R"([{"op":"csv.set","file":"t.csv","match":"zz","set":{"Speed":1}}])"));
        CHECK(Fails(src, R"([{"op":"csv.set","file":"t.csv","match":"a","set":{"Nope":1}}])"));
    }
    // text: exact match, ambiguity, CRLF, unknown field
    {
        MemorySource src;
        src.files["x"] = B("one\r\ntwo\r\none\r\n");
        CHECK(Fails(src, R"([{"op":"text.replace","file":"x","find":"one","replace":"1"}])"));  // twice
        CHECK(Run(src, "x", R"([{"op":"text.replace","file":"x","find":"one\ntwo","replace":"1\n2"}])") == "1\r\n2\r\none\r\n");
        CHECK(Run(src, "x", R"([{"op":"text.replace","file":"x","find":"one","replace":"1","all":true}])") == "1\r\ntwo\r\n1\r\n");
        CHECK(Fails(src, R"([{"op":"text.replace","file":"x","fnd":"one","replace":"1"}])"));
        CHECK(Run(src, "x", R"([{"op":"text.insert","file":"x","after":"two","text":"!"}])") == "one\r\ntwo!\r\none\r\n");
    }
    // XML: attribute edits keep everything else
    {
        const std::string xml = "<?xml version=\"1.0\"?>\n<A>\n  <!-- c -->\n  <B n=\"1\" v=\"x\"/>\n  <B n=\"2\"></B>\n</A>\n";
        MemorySource src;
        src.files["x"] = B(xml);
        CHECK(Run(src, "x", R"([{"op":"xml.set","file":"x","path":"A/B[@n='1']","attr":"v","value":"y"}])") ==
              "<?xml version=\"1.0\"?>\n<A>\n  <!-- c -->\n  <B n=\"1\" v=\"y\"/>\n  <B n=\"2\"></B>\n</A>\n");
        CHECK(Run(src, "x", R"([{"op":"xml.set","file":"x","path":"A/B[2]","attr":"w","value":"3"}])").find("<B n=\"2\" w=\"3\">") != std::string::npos);
        CHECK(Run(src, "x", R"([{"op":"xml.remove","file":"x","path":"A/B[@n='1']"}])") == "<?xml version=\"1.0\"?>\n<A>\n  <!-- c -->\n  <B n=\"2\"></B>\n</A>\n");
        CHECK(Run(src, "x", R"([{"op":"xml.insert","file":"x","path":"A","xml":"<C/>"}])").find("  <C/>\n</A>") != std::string::npos);
        CHECK(Fails(src, R"([{"op":"xml.set","file":"x","path":"A/B","attr":"v","value":"y"}])"));  // two matches
    }
    // JSON merge, bytes, file ops
    {
        MemorySource src;
        src.files["j"] = B("{\"a\":1,\"b\":{\"c\":2}}");
        CHECK(Json::Parse(Run(src, "j", R"([{"op":"json.merge","file":"j","value":{"b":{"c":3,"d":4},"a":null}}])")).Find("b")->GetInt("c") == 3);
        src.files["bin"] = B("abcd");
        CHECK(Run(src, "bin", R"([{"op":"bytes.patch","file":"bin","offset":1,"expect":"6263","write":"5858"}])") == "aXXd");
        CHECK(Fails(src, R"([{"op":"bytes.patch","file":"bin","offset":1,"expect":"0000","write":"5858"}])"));
        CHECK(Run(src, "copy", R"([{"op":"file.copy","from":"bin","file":"copy"}])") == "abcd");
    }
    // packs: a resource is replaced and the pack still reads
    {
        RpkPack pack;
        pack.info_id = (1ull << 48) | 5;
        RpkResource r;
        r.id = (39ull << 48) | 7;
        r.names = {"conf.xml"};
        r.sources = {""};
        r.data = RpkResource::MakeBlobPayload(r.id, 0x10000, B("<a x=\"1\"/>"));
        pack.resources.push_back(r);
        pack.root = r.id;
        MemorySource src;
        src.files["p.rpk"] = pack.Write();
        const std::string out = Run(src, "p.rpk", R"([{"op":"rpk.patch","file":"p.rpk","resource":"conf.xml","ops":[{"op":"xml.set","path":"a","attr":"x","value":"22"}]}])");
        const RpkPack back = RpkPack::Parse(B(out));
        CHECK(S(back.resources[0].Blob()) == "<a x=\"22\"/>");
        CHECK(RpkPack::Parse(src.files["p.rpk"]).Write() == src.files["p.rpk"]);
    }
    // two mods that set the same cell are reported
    {
        MemorySource src;
        src.files["t.csv"] = B("K,V\na,1\n");
        PatchReport report;
        Workspace ws(src, report);
        PatchEnv e1{ws, "one", "."}, e2{ws, "two", "."};
        ApplyPatches(Json::Parse(R"([{"op":"csv.set","file":"t.csv","match":"a","set":{"V":2}}])"), e1);
        ApplyPatches(Json::Parse(R"([{"op":"csv.set","file":"t.csv","match":"a","set":{"V":3}}])"), e2);
        CHECK(report.conflicts.size() == 1);
        CHECK(S(ws.Finish().at("t.csv")) == "K,V\na,3\n");
    }
    std::printf(failures ? "%d checks FAILED\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
