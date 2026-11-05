// SPDX-License-Identifier: GPL-2.0-or-later
// Checks of the mod kit on made-up data (no game files needed).
#include "../src/csv.hpp"
#include "../src/diff.hpp"
#include "../src/patch.hpp"
#include "../src/tex.hpp"
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
    // textures: tiling and untiling are inverse, a changed DDS reaches the pack, wrong sizes are refused
    {
        Bytes payload{'E', 'V', 'O', 'S', 'L', 'I', 'T', 'L'};
        Wr64(payload, (5ull << 48) | 9);
        Wr32(payload, 0x20005);
        Wr16(payload, 20); Wr16(payload, 12); Wr16(payload, 1); Wr16(payload, 36);  // 20x12 BC7
        payload.insert(payload.end(), {2, 0, 1, 0, 0, 0});                          // 2 mips, type, usage, unk, start, unk3
        Wr64(payload, 0);
        Wr32(payload, 0);
        PixelBuffer pb = PixelBuffer::Parse(payload);
        Bytes data(pb.ExpectedDataSize());
        for (size_t i = 0; i < data.size(); ++i) data[i] = uint8_t(i * 7 + i / 251);
        pb.SetData(data);
        pb = PixelBuffer::Parse(pb.payload);
        const Dds dds = TexToDds(pb);
        CHECK(dds.mips.size() == 2 && dds.mips[0].size() == 5 * 3 * 16 && dds.mips[1].size() == 3 * 2 * 16);
        const Dds again = Dds::Parse(dds.Write());
        CHECK(again.mips == dds.mips && again.width == 20);
        Bytes retiled = DdsToTexData(pb, again);
        // padding bytes are not kept, so compare the untiled content
        PixelBuffer pb2 = pb;
        pb2.SetData(retiled);
        CHECK(TexToDds(pb2).mips == dds.mips);
        Dds wrong = again;
        wrong.width = 16;
        bool refused = false;
        try { DdsToTexData(pb, wrong); } catch (const Error&) { refused = true; }
        CHECK(refused);
    }
    // diff: the patches made from two versions of a file turn the first into the second
    {
        const std::string a = "a,1\nb,2\nc,3\nd,4\ne,5\nf,6\ng,7\nh,8\nrepeat\nrepeat\n";
        const std::string b = "a,1\nb,22\nc,3\nd,4\ne,5\nf,6\nNEW\ng,7\nh,8\nrepeat\nrepeat\nlast\n";
        const auto ops = DiffText("x.txt", B(a), B(b));
        CHECK(ops && ops->size() >= 2);
        MemorySource src;
        src.files["x.txt"] = B(a);
        Json list = Json::Array();
        for (const Json& op : *ops) list.Push(op);
        CHECK(Run(src, "x.txt", list.Dump()) == b);
        const std::string t1 = "Id,V\n1,a\n2,b\n3,c\n", t2 = "Id,V\n1,a\n2,x\n4,d\n";
        src.files["t.csv"] = B(t1);
        const auto cops = DiffText("t.csv", B(t1), B(t2));
        CHECK(cops.has_value());
        Json clist = Json::Array();
        for (const Json& op : *cops) clist.Push(op);
        CHECK(Run(src, "t.csv", clist.Dump()) == t2);
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
