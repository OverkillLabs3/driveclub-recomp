// SPDX-License-Identifier: GPL-2.0-or-later
// dcmod dash ...: the in-car dashboards. Every car has an XML description of its instruments (assets/dashboards/*.def,
// stored as binary resources in global.rpk) that draws pictures from a texture atlas and shows live values of the car
// (speed, revs, gear, lap times, temperatures, ...) as numbers, needles, arcs, bars and lights.
#include "cli.hpp"
#include "rpk.hpp"
#include "textfile.hpp"
#include "xml.hpp"

#include <functional>
#include <map>
#include <set>

namespace dcmod {

namespace {

// Elements and the attributes they take, from the 125 dashboards of the game.
const std::map<std::string, std::set<std::string>>& Elements() {
    static const std::map<std::string, std::set<std::string>> m = {
        {"dashboard", {"xmlns", "xmlns:xsi", "xsi:schemaLocation"}},
        {"canvas", {"dw", "dh"}},
        {"font", {"name", "sx", "sy", "sw", "sh", "kerning", "glyphs", "densepacking", "widths"}},
        {"number", {"font", "dx", "dy", "align", "decimals", "digits", "kerning", "leadingzeros", "monospace", "multiplier", "value"}},
        {"bind", {"value", "alphavalue", "fractions", "minutes", "seconds"}},
        {"visible", {"bind", "min", "max"}},
        {"image", {"sx", "sy", "sw", "sh", "dx", "dy", "ox", "oy", "value", "minvalue", "maxvalue", "minangle", "maxangle", "ranges"}},
        {"maskedimage", {"sw", "sh", "dx", "dy", "ax", "ay", "bx", "by", "mx", "my", "minvalue", "maxvalue"}},
        {"fadeimage", {"sx", "sy", "sw", "sh", "dx", "dy", "alpha_ranges"}},
        {"bespokeimage", {"x", "y", "w", "h"}},
        {"slidebox", {"sx", "sy", "sw", "sh", "dx", "dy", "invertx", "inverty", "mirrorx"}},
        {"slider", {"sx", "sy", "sw", "sh", "minx", "maxx", "miny", "maxy", "minvalue", "maxvalue"}},
        {"needle", {"name", "minvalue", "maxvalue", "minangle", "maxangle", "ranges"}},
        {"arc", {"dx", "dy", "dr", "dw", "colour", "minvalue", "maxvalue", "minangle", "maxangle"}},
        {"box", {"dx", "dy", "dw", "dh", "colour", "opacity"}},
        {"line", {"colour", "opacity", "thickness"}},
        {"gear", {"font", "dx", "dy", "align"}},
        {"timer", {"clock", "font", "dx", "dy", "kerning", "monospace", "fractions", "fractiondigits", "display_diff"}}};
    return m;
}

// Values the shipped dashboards read, by name.
const std::vector<std::string>& Binds() {
    static const std::vector<std::string> v = {
        "ambient.date_day", "ambient.date_month", "ambient.date_year", "ambient.temperature_c", "ambient.temperature_f",
        "ambient.time_hours", "ambient.time_minutes", "ambient.time_seconds",
        "vehicle.air_temp", "vehicle.bac_mono_rev_flash", "vehicle.battery", "vehicle.battery_percent", "vehicle.bhp",
        "vehicle.best_lap_ff", "vehicle.best_lap_mm", "vehicle.best_lap_ss", "vehicle.boost_percent", "vehicle.boost_vacuum_percent",
        "vehicle.brake_percent", "vehicle.braking_gforce_percent", "vehicle.current_lap", "vehicle.current_lap_ff",
        "vehicle.current_lap_mm", "vehicle.current_lap_ss", "vehicle.current_lean_angle", "vehicle.current_lean_angle_abs",
        "vehicle.drs_percent", "vehicle.electric_bhp_percent", "vehicle.fuel_litres", "vehicle.fuel_percent",
        "vehicle.fuel_pressure", "vehicle.gear", "vehicle.handbrake", "vehicle.headlights", "vehicle.kers_discharge",
        "vehicle.kers_percent", "vehicle.kers_recharge", "vehicle.last_lap_ff", "vehicle.last_lap_mm", "vehicle.last_lap_ss",
        "vehicle.lateralG", "vehicle.laps_remaining", "vehicle.longitudinalG", "vehicle.max_boost_percent",
        "vehicle.max_braking_gforce_percent", "vehicle.max_left_lean_angle", "vehicle.max_right_lean_angle",
        "vehicle.max_speed_kph_best_lap", "vehicle.max_speed_kph_last_lap", "vehicle.max_traction_control_percent",
        "vehicle.mileage_kilometres", "vehicle.mileage_miles", "vehicle.min_speed_kph_best_lap", "vehicle.min_speed_kph_last_lap",
        "vehicle.oil_pressure", "vehicle.oil_pressure_percent", "vehicle.oil_temp", "vehicle.oil_temp_percent",
        "vehicle.output_watts", "vehicle.revs", "vehicle.revs_percent", "vehicle.seat_belt", "vehicle.speed_kph",
        "vehicle.speed_mph", "vehicle.steering_percent", "vehicle.throttle_percent", "vehicle.traction_control",
        "vehicle.traction_control_enabled", "vehicle.traction_control_percent", "vehicle.trip_mileage_kilometres",
        "vehicle.trip_mileage_miles", "vehicle.water_temp", "vehicle.water_temp_percent", "vehicle.wheel_power_watts_bl",
        "vehicle.wheel_power_watts_br", "vehicle.wheel_power_watts_fl", "vehicle.wheel_power_watts_fr"};
    return v;
}

int Usage() {
    std::puts(
        "dcmod dash: in-car dashboards (the .def files in global.rpk)\n"
        "\n"
        "  dcmod dash list                    the dashboards of the game's cars\n"
        "  dcmod dash cat NAME                one dashboard's XML (NAME as in the list, with or without .def)\n"
        "  dcmod dash check FILE|NAME         check a dashboard: elements, attributes and live values the game knows\n"
        "  dcmod dash binds                   the live values dashboards can show\n"
        "\n"
        "To use a changed dashboard, put it in a mod: rpk.set on global.rpk with the resource name, or rpk.patch with\n"
        "xml.* operations. The pictures a dashboard draws come from the atlas texture in the car's pack (tex.set).");
    return 2;
}

Bytes DashBytes(const Args& args, const std::string& name) {
    auto archive = OpenGame(args);
    const IndexEntry* e = archive->Find("global.rpk");
    if (!e) throw Error("global.rpk is not in the game's archive");
    const RpkPack pack = RpkPack::Parse(archive->Read(*e));
    std::string want = Lower(name);
    if (!EndsWith(want, ".def")) want += ".def";
    for (const RpkResource& r : pack.resources) {
        if (r.Type() == kRpkBin && !r.names.empty() && Lower(r.names.front()) == want) return r.Blob();
    }
    throw Error("no dashboard named " + name + "; 'dcmod dash list' shows them");
}

int Check(const std::string& text, bool print_ok) {
    const TextFile tf = TextFile::Load(Bytes(text.begin(), text.end()));
    const XmlDoc doc = XmlDoc::Parse(tf.text);
    const auto& elements = Elements();
    const std::set<std::string> binds(Binds().begin(), Binds().end());
    int problems = 0, warnings = 0;
    bool canvas = false;
    const std::function<void(size_t)> walk = [&](size_t i) {
        const XmlNode& n = doc.node(i);
        if (i != 0) {
            const auto it = elements.find(n.name);
            if (it == elements.end()) {
                std::printf("error: <%s> is not a dashboard element\n", n.name.c_str());
                ++problems;
            } else {
                if (n.name == "canvas") canvas = true;
                for (const XmlAttr& a : n.attrs) {
                    if (!it->second.count(a.name)) {
                        std::printf("error: <%s> has no attribute %s\n", n.name.c_str(), a.name.c_str());
                        ++problems;
                    }
                    if ((a.name == "bind" || (n.name == "bind" && a.name == "value") || (n.name == "number" && a.name == "value")) && a.value.find('.') != std::string::npos &&
                        std::isalpha(static_cast<unsigned char>(a.value[0])) && !binds.count(a.value)) {
                        std::printf("warning: \"%s\" is not a value any dashboard of the game shows\n", a.value.c_str());
                        ++warnings;
                    }
                }
            }
        }
        for (size_t c : n.children) walk(c);
    };
    walk(0);
    if (!canvas) {
        std::puts("error: there is no <canvas dw=\"...\" dh=\"...\"/>");
        ++problems;
    }
    if (print_ok || problems || warnings) std::printf("%d errors, %d warnings\n", problems, warnings);
    return problems ? 1 : 0;
}

}  // namespace

int RunDash(const std::vector<std::string>& argv) {
    if (argv.empty()) return Usage();
    const std::string cmd = argv[0];
    const Args args = ParseArgs(argv, {"--update", "--base", "--game"});
    if (cmd == "binds") {
        for (const std::string& b : Binds()) std::printf("%s\n", b.c_str());
        return 0;
    }
    if (cmd == "list") {
        auto archive = OpenGame(args);
        const IndexEntry* e = archive->Find("global.rpk");
        if (!e) throw Error("global.rpk is not in the game's archive");
        const RpkPack pack = RpkPack::Parse(archive->Read(*e));
        for (const RpkResource& r : pack.resources) {
            if (r.Type() == kRpkBin && !r.names.empty() && EndsWith(Lower(r.names.front()), ".def")) std::printf("%s\n", r.names.front().c_str());
        }
        return 0;
    }
    if (args.positional.size() < 2) return Usage();
    if (cmd == "cat") {
        const Bytes b = DashBytes(args, args.positional[1]);
        std::fwrite(b.data(), 1, b.size(), stdout);
        return 0;
    }
    if (cmd == "check") {
        const std::string& what = args.positional[1];
        Bytes b;
        if (fs::exists(what)) b = ReadFile(what);
        else b = DashBytes(args, what);
        return Check(std::string(b.begin(), b.end()), true);
    }
    return Usage();
}

}  // namespace dcmod
