#include <aaf/preview/preview.hpp>
#include <aaf/timeline/timeline.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <ranges>

namespace aaf::preview
{

namespace
{

using timeline::Item;
using timeline::ItemKind;
using timeline::MobKind;
using timeline::MobSummary;
using timeline::Track;
using timeline::TrackKind;

constexpr std::array kZoomLevels = { 1, 2, 4, 8, 16 };
constexpr double kMergeBelowPercent = 0.04;
constexpr std::size_t kDrawBudget = 12000;
constexpr std::size_t kMaxDrawnPerTrack = 4000;
constexpr std::size_t kMaxCompositionsListed = 50;

auto text(const Document& doc, ObjectId id, std::string_view cls, std::string_view property) -> std::string
{
    const auto v = doc.value(id, cls, property);
    return v && v->is<std::string>() ? v->as<std::string>() : std::string{};
}

auto recordField(const Value& v, std::string_view name) -> const Value*
{
    if (!v.is<Value::Record>())
    {
        return nullptr;
    }
    const auto& r = v.as<Value::Record>();
    for (std::size_t i = 0; i < r.names.size(); ++i)
    {
        if (r.names[i] == name)
        {
            return &r.values[i];
        }
    }
    return nullptr;
}

auto integer(const Value* v) -> std::int64_t
{
    if (v == nullptr)
    {
        return 0;
    }
    if (v->is<std::int64_t>())
    {
        return v->as<std::int64_t>();
    }
    if (v->is<std::uint64_t>())
    {
        return static_cast<std::int64_t>(v->as<std::uint64_t>());
    }
    return 0;
}

/// "2024-03-05 14:07" from an AAF TimeStamp record, or empty.
auto formatTimestamp(const std::optional<Value>& v) -> std::string
{
    if (!v)
    {
        return {};
    }
    const auto* date = recordField(*v, "date");
    const auto* time = recordField(*v, "time");
    if (date == nullptr || time == nullptr)
    {
        return {};
    }
    const auto year = integer(recordField(*date, "year"));
    if (year <= 0)
    {
        return {};
    }
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}", year, integer(recordField(*date, "month")), integer(recordField(*date, "day")), integer(recordField(*time, "hour")), integer(recordField(*time, "minute")));
}

auto formatBytes(std::uintmax_t size) -> std::string
{
    constexpr std::array units = { "bytes", "KB", "MB", "GB", "TB" };
    auto value = static_cast<double>(size);
    std::size_t unit = 0;
    while (value >= 1000.0 && unit + 1 < units.size())
    {
        value /= 1000.0;
        ++unit;
    }
    return unit == 0 ? std::format("{} bytes", size) : std::format("{:.1f} {}", value, units[unit]);
}

auto formatCount(std::size_t n, std::string_view singular, std::string_view plural) -> std::string
{
    return std::format("{} {}", n, n == 1 ? singular : plural);
}

struct Identity
{
    std::string product;
    std::string company;
    std::string platform;
    std::string date;
};

/// The Identification records in Header.IdentificationList: the first wrote the file, the last modified it.
auto identities(const Document& doc) -> std::vector<Identity>
{
    std::vector<Identity> out;
    const auto header = doc.header();
    const auto* list = header == kNoObject ? nullptr : doc.model().findProperty("Header", "IdentificationList");
    const auto* p = list == nullptr ? nullptr : doc.object(header).find(list->pid);
    const auto* v = p == nullptr ? nullptr : std::get_if<StrongRefVectorProperty>(&p->payload);
    if (v == nullptr)
    {
        return out;
    }
    for (const auto id : v->objects)
    {
        Identity i;
        i.product = text(doc, id, "Identification", "ProductName");
        const auto version = text(doc, id, "Identification", "ProductVersionString");
        if (!version.empty() && version != "Unknown version")
        {
            i.product += (i.product.empty() ? "" : " ") + version;
        }
        i.company = text(doc, id, "Identification", "CompanyName");
        i.platform = text(doc, id, "Identification", "Platform");
        i.date = formatTimestamp(doc.value(id, "Identification", "Date"));
        out.push_back(std::move(i));
    }
    return out;
}

auto describe(const Identity& i) -> std::string
{
    auto out = i.product.empty() ? std::string("Unknown application") : i.product;
    if (!i.company.empty() && !out.contains(i.company))
    {
        out = i.company + " " + out;
    }
    if (!i.platform.empty())
    {
        out += " (" + i.platform + ")";
    }
    return out;
}

auto trackPrefix(TrackKind kind) -> std::string_view
{
    switch (kind)
    {
        case TrackKind::picture:
            return "V";
        case TrackKind::sound:
            return "A";
        case TrackKind::timecode:
            return "TC";
        case TrackKind::edgecode:
            return "EC";
        case TrackKind::descriptiveMetadata:
            return "DM";
        case TrackKind::data:
            return "D";
        case TrackKind::other:
            return "?";
    }
    return "?";
}

/// "V1", "A2": picture and sound tracks are always numbered, the others only from the second one.
auto laneLabel(TrackKind kind, int ordinal) -> std::string
{
    const bool numbered = kind == TrackKind::picture || kind == TrackKind::sound || ordinal > 1;
    return numbered ? std::format("{}{}", trackPrefix(kind), ordinal) : std::string(trackPrefix(kind));
}

auto trackOrder(TrackKind kind) -> int
{
    switch (kind)
    {
        case TrackKind::picture:
            return 0;
        case TrackKind::sound:
            return 1;
        case TrackKind::descriptiveMetadata:
            return 2;
        default:
            return 3;
    }
}

struct Lane
{
    const Track* track = nullptr;
    std::string label;
    double scale = 1.0;
};

/// Converts a track's edit units to the base rate.
auto rateScale(const timeline::Rational& base, const timeline::Rational& rate) -> double
{
    const auto r = rate.toDouble();
    return r > 0 ? base.toDouble() / r : 1.0;
}

auto clipClass(const Item& item, TrackKind kind) -> std::string_view
{
    if (item.kind == ItemKind::sourceClip || item.clip)
    {
        if (item.source && !item.source->mob && !item.source->original)
        {
            return "missing";
        }
        if (item.source && item.source->mobKind == MobKind::composition)
        {
            return "nested";
        }
        return kind == TrackKind::sound ? "a" : "v";
    }
    switch (item.kind)
    {
        case ItemKind::operationGroup:
            return "fx";
        case ItemKind::nestedScope:
        case ItemKind::sequence:
        case ItemKind::selector:
        case ItemKind::essenceGroup:
            return "nested";
        default:
            return "other";
    }
}

auto itemLabel(const Item& item) -> std::string
{
    if (!item.label.empty())
    {
        return item.label;
    }
    if (item.source && !item.source->mobName.empty())
    {
        return item.source->mobName;
    }
    if (!item.effect.empty())
    {
        return item.effect;
    }
    return item.className;
}

auto effectNames(const Item& item) -> std::string
{
    std::string out;
    for (const auto& e : item.effects)
    {
        out += (out.empty() ? "" : ", ") + e.name;
    }
    if (out.empty() && item.kind == ItemKind::operationGroup)
    {
        out = item.effect;
    }
    return out;
}

class Clock
{
public:
    Clock(timeline::Rational base, std::optional<timeline::Timecode> tc) :
        base_(base),
        fps_(tc && tc->fps > 0 ? tc->fps : static_cast<std::uint32_t>(std::lround(base.toDouble()))),
        drop_(tc && tc->drop),
        start_(tc ? tc->start : 0)
    {
    }

    [[nodiscard]] auto at(double position) const -> std::string { return formatTimecode(start_ + std::llround(position), fps_, drop_); }
    [[nodiscard]] auto duration(double length) const -> std::string { return formatTimecode(std::llround(length), fps_, drop_); }
    [[nodiscard]] auto fps() const -> std::uint32_t { return fps_; }
    [[nodiscard]] auto base() const -> const timeline::Rational& { return base_; }

private:
    timeline::Rational base_;
    std::uint32_t fps_;
    bool drop_;
    std::int64_t start_;
};

auto percent(double value) -> std::string
{
    return std::format("{:.4f}%", value);
}

/// A ruler tick step in base edit units giving about `ticks` ticks across `total`.
auto tickStep(double total, double fps, int ticks) -> double
{
    const double seconds = fps > 0 ? fps : 1.0;
    constexpr std::array steps = { 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0, 900.0, 1800.0, 3600.0 };
    for (const auto s : steps)
    {
        if (total / (s * seconds) <= ticks)
        {
            return s * seconds;
        }
    }
    return steps.back() * seconds * std::ceil(total / (steps.back() * seconds * ticks));
}

class Writer
{
public:
    Writer(const Document& doc, const PreviewOptions& options) :
        doc_(doc),
        options_(options),
        projector_(doc)
    {
    }

    auto render() -> std::string
    {
        const auto mobs = projector_.mobs();
        out_ += "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>" + escapeHtml(options_.fileName) + "</title><style>";
        out_ += kStyle;
        for (const auto z : kZoomLevels)
        {
            out_ += std::format("#z{0}:checked~.zoom label[for=z{0}]{{background:var(--fg);color:var(--bg);border-color:var(--fg)}}#z{0}:checked~.tl .inner{{width:{1}%}}#z{0}:checked~.tl .ruler.z{0}{{display:block}}", z, z * 100);
        }
        out_ += "</style></head><body>";
        header(mobs);
        const auto main = mainComposition(mobs);
        if (main)
        {
            composition(*main);
        }
        otherCompositions(mobs, main);
        mobList(mobs, MobKind::master, "Master clips");
        mobList(mobs, MobKind::source, "Sources");
        out_ += "</body></html>";
        return std::move(out_);
    }

private:
    static constexpr std::string_view kStyle =
        ":root{color-scheme:light dark;--bg:#fff;--fg:#1d1d1f;--muted:#6e6e73;--line:#d2d2d7;--lane:#f2f2f5;--v:#3478f6;--a:#2fb67c;--fx:#8e6cf0;--nested:#e8912d;--missing:#e5484d;--other:#8e8e93;--tr:rgba(0,0,0,.28)}"
        "@media (prefers-color-scheme:dark){:root{--bg:#1e1e1e;--fg:#f5f5f7;--muted:#98989d;--line:#3a3a3c;--lane:#2a2a2c;--tr:rgba(255,255,255,.35)}}"
        "*{box-sizing:border-box}"
        "body{margin:0;padding:16px 20px;background:var(--bg);color:var(--fg);font:12px -apple-system,BlinkMacSystemFont,\"Helvetica Neue\",\"Segoe UI\",sans-serif}"
        "h1{font-size:18px;margin:0 0 8px;overflow-wrap:anywhere}"
        "h2{font-size:13px;margin:22px 0 6px;color:var(--muted);text-transform:uppercase;letter-spacing:.04em}"
        "h3{font-size:14px;margin:4px 0 6px;overflow-wrap:anywhere}"
        ".summary{display:flex;flex-wrap:wrap;gap:6px 24px;margin:0}"
        ".summary dt{color:var(--muted);font-size:11px}"
        ".summary dd{margin:0;font-variant-numeric:tabular-nums}"
        ".muted{color:var(--muted)}"
        ".timeline{margin-top:12px;position:relative}"
        ".timeline>input{position:absolute;opacity:0;pointer-events:none}"
        ".zoom{display:flex;justify-content:flex-end;gap:2px;margin-bottom:4px}"
        ".zoom label{font-size:10px;color:var(--muted);padding:1px 7px;border:1px solid var(--line);border-radius:4px;cursor:pointer;user-select:none}"
        ".tl{display:flex;border:1px solid var(--line);border-radius:6px;overflow:hidden}"
        ".labels{flex:0 0 auto;border-right:1px solid var(--line);padding-top:18px}"
        ".labels div{height:24px;line-height:24px;padding:0 8px;font-weight:600;color:var(--muted);font-size:11px}"
        ".scroll{flex:1 1 auto;min-width:0;overflow-x:auto;overflow-y:hidden}"
        ".inner{width:100%;position:relative}"
        ".ruler{display:none;position:relative;height:18px;border-bottom:1px solid var(--line)}"
        ".ruler span{position:absolute;top:2px;font-size:9px;color:var(--muted);padding-left:3px;border-left:1px solid var(--line);white-space:nowrap;font-variant-numeric:tabular-nums}"
        ".lane{position:relative;height:24px;background:var(--lane);border-bottom:1px solid var(--bg)}"
        ".c{position:absolute;top:3px;height:18px;border-radius:3px;color:#fff;font-size:10px;line-height:18px;padding:0 4px;overflow:hidden;white-space:nowrap;text-overflow:clip;min-width:1px;box-shadow:inset -1px 0 0 rgba(0,0,0,.2)}"
        ".c.v{background:var(--v)}.c.a{background:var(--a)}.c.fx{background:var(--fx)}.c.nested{background:var(--nested)}.c.missing{background:var(--missing)}.c.other{background:var(--other)}"
        ".c i{font-style:normal;opacity:.85;margin-right:3px}"
        ".t{position:absolute;top:0;height:24px;background:repeating-linear-gradient(135deg,var(--tr) 0 3px,transparent 3px 6px)}"
        ".m{position:absolute;top:4px;width:8px;height:8px;margin-left:-4px;background:#ff9500;transform:rotate(45deg)}"
        "table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums}"
        "th{text-align:left;color:var(--muted);font-weight:500;font-size:11px;border-bottom:1px solid var(--line);padding:3px 8px 3px 0;position:sticky;top:0;background:var(--bg)}"
        "td{padding:2px 8px 2px 0;border-bottom:1px solid var(--lane);vertical-align:top;overflow-wrap:anywhere}"
        "td.tc{white-space:nowrap;font-family:ui-monospace,Menlo,monospace;font-size:11px}"
        ".tag{display:inline-block;font-size:10px;padding:0 5px;border-radius:3px;background:var(--lane);color:var(--muted);margin-left:4px}"
        ".err{color:var(--missing)}";

    void item(std::string_view term, std::string_view value)
    {
        out_ += "<div><dt>" + escapeHtml(term) + "</dt><dd>" + escapeHtml(value) + "</dd></div>";
    }

    void header(const std::vector<MobSummary>& mobs)
    {
        out_ += "<header><h1>" + escapeHtml(options_.fileName) + "</h1><dl class=\"summary\">";
        const auto ids = identities(doc_);
        if (!ids.empty())
        {
            item("Created by", describe(ids.front()) + (ids.front().date.empty() ? "" : ", " + ids.front().date));
            if (ids.size() > 1)
            {
                item("Last modified by", describe(ids.back()) + (ids.back().date.empty() ? "" : ", " + ids.back().date));
            }
        }
        std::map<MobKind, std::size_t> counts;
        for (const auto& m : mobs)
        {
            ++counts[m.kind];
        }
        item("Compositions", std::to_string(counts[MobKind::composition]));
        item("Master clips", std::to_string(counts[MobKind::master]));
        item("Sources", std::to_string(counts[MobKind::source]));
        item("Objects", std::to_string(doc_.objectCount()));
        item("Container", doc_.container().header().version == cfb::Version::v3 ? "AAF (512-byte sectors)" : "AAF (4 KB sectors)");
        if (options_.fileSize)
        {
            item("Size", formatBytes(*options_.fileSize));
        }
        out_ += "</dl></header>";
    }

    static auto mainComposition(const std::vector<MobSummary>& mobs) -> std::optional<MobSummary>
    {
        const MobSummary* best = nullptr;
        for (const auto& m : mobs)
        {
            if (m.kind == MobKind::composition && m.topLevel && (best == nullptr || m.tracks > best->tracks))
            {
                best = &m;
            }
        }
        if (best != nullptr)
        {
            return *best;
        }
        const auto any = std::ranges::find_if(mobs, [](const MobSummary& m) -> bool { return m.kind == MobKind::composition; });
        return any == mobs.end() ? std::nullopt : std::optional(*any);
    }

    void composition(const MobSummary& summary)
    {
        auto projected = projector_.project(summary.object);
        out_ += "<h2>Composition</h2><h3>" + escapeHtml(summary.name.empty() ? "Untitled" : summary.name) + "</h3>";
        if (!projected)
        {
            out_ += "<p class=\"err\">" + escapeHtml(projected.error().message) + "</p>";
            return;
        }
        const auto& t = *projected;
        std::vector<Lane> lanes;
        std::map<TrackKind, int> ordinals;
        const Track* baseTrack = nullptr;
        for (const auto& track : t.tracks)
        {
            if (track.slotKind == timeline::SlotKind::fixed)
            {
                continue;
            }
            if (baseTrack == nullptr && track.kind == TrackKind::picture && track.slotKind == timeline::SlotKind::timeline)
            {
                baseTrack = &track;
            }
        }
        if (baseTrack == nullptr)
        {
            const auto first = std::ranges::find_if(t.tracks, [](const Track& tr) -> bool { return tr.slotKind == timeline::SlotKind::timeline; });
            baseTrack = first == t.tracks.end() ? nullptr : &*first;
        }
        const auto base = baseTrack != nullptr ? baseTrack->editRate : timeline::Rational(25, 1);
        for (const auto& track : t.tracks)
        {
            if (track.kind == TrackKind::timecode || track.kind == TrackKind::edgecode || track.slotKind == timeline::SlotKind::fixed)
            {
                continue;
            }
            const auto n = ++ordinals[track.kind];
            lanes.push_back({ &track, laneLabel(track.kind, n), rateScale(base, track.editRate) });
        }
        std::ranges::stable_sort(lanes, {}, [](const Lane& l) -> int { return trackOrder(l.track->kind); });

        double total = 0;
        for (const auto& lane : lanes)
        {
            total = std::max(total, static_cast<double>(lane.track->length) * lane.scale);
            for (const auto& i : lane.track->items)
            {
                total = std::max(total, static_cast<double>(i.start + (i.hasLength ? i.length : 0)) * lane.scale);
            }
        }
        const Clock clock(base, t.timecode);

        out_ += "<dl class=\"summary\">";
        item("Start", clock.at(0));
        item("Duration", clock.duration(total));
        item("Edit rate", std::format("{} fps", base.toString()));
        std::string trackNames;
        for (const auto& lane : lanes)
        {
            trackNames += (trackNames.empty() ? "" : " ") + lane.label;
        }
        item("Tracks", trackNames.empty() ? "none" : trackNames);
        out_ += "</dl>";
        if (!t.warnings.empty())
        {
            out_ += "<p class=\"muted\">" + escapeHtml(formatCount(t.warnings.size(), "warning", "warnings")) + ": " + escapeHtml(t.warnings.front()) + "</p>";
        }
        if (!lanes.empty() && total > 0)
        {
            timelineView(lanes, total, clock);
            clipList(lanes, clock);
        }
    }

    void timelineView(const std::vector<Lane>& lanes, double total, const Clock& clock)
    {
        out_ += "<div class=\"timeline\">";
        for (const auto z : kZoomLevels)
        {
            out_ += std::format(R"(<input type="radio" name="zoom" id="z{0}"{1}>)", z, z == 1 ? " checked" : "");
        }
        out_ += "<div class=\"zoom\">";
        for (const auto z : kZoomLevels)
        {
            out_ += std::format("<label for=\"z{0}\">{0}×</label>", z);
        }
        out_ += R"(</div><div class="tl"><div class="labels">)";
        for (const auto& lane : lanes)
        {
            out_ += "<div>" + escapeHtml(lane.label) + "</div>";
        }
        out_ += R"(</div><div class="scroll"><div class="inner">)";
        for (const auto z : kZoomLevels)
        {
            out_ += std::format("<div class=\"ruler z{}\">", z);
            const auto step = tickStep(total, clock.fps(), 8 * z);
            for (std::int64_t k = 0; static_cast<double>(k) * step < total; ++k)
            {
                const auto p = static_cast<double>(k) * step;
                out_ += R"(<span style="left:)" + percent(p / total * 100) + R"(">)" + clock.at(p) + "</span>";
            }
            out_ += "</div>";
        }
        std::size_t items = 0;
        for (const auto& lane : lanes)
        {
            items += lane.track->items.size();
        }
        const auto mergeBelow = kMergeBelowPercent * std::max(1.0, static_cast<double>(items) / kDrawBudget);
        for (const auto& lane : lanes)
        {
            out_ += "<div class=\"lane\">";
            drawLane(lane, total, mergeBelow);
            out_ += "</div>";
        }
        out_ += "</div></div></div></div>";
    }

    void drawLane(const Lane& lane, double total, double mergeBelow)
    {
        std::size_t drawn = 0;
        struct Run
        {
            double start = 0;
            double end = 0;
            std::string_view cls;
            std::size_t count = 0;
        };
        std::optional<Run> run;
        const auto flush = [&] -> void {
            if (run)
            {
                out_ += std::format(R"(<div class="c {}" style="left:{};width:{}" title="{} items"></div>)", run->cls, percent(run->start), percent(run->end - run->start), run->count);
                ++drawn;
                run.reset();
            }
        };
        for (const auto& i : lane.track->items)
        {
            if (drawn >= kMaxDrawnPerTrack)
            {
                break;
            }
            const auto left = static_cast<double>(i.start) * lane.scale / total * 100;
            const auto width = static_cast<double>(i.hasLength ? i.length : 0) * lane.scale / total * 100;
            if (i.kind == ItemKind::filler)
            {
                continue;
            }
            if (i.kind == ItemKind::transition)
            {
                flush();
                out_ += std::format(R"(<div class="t" style="left:{};width:{}" title="{}"></div>)", percent(left), percent(width), escapeHtml(i.effect.empty() ? "Transition" : i.effect));
                ++drawn;
                continue;
            }
            if (i.kind == ItemKind::marker || i.kind == ItemKind::event || !i.hasLength)
            {
                flush();
                out_ += std::format(R"(<div class="m" style="left:{}" title="{}"></div>)", percent(left), escapeHtml(i.comment.empty() ? itemLabel(i) : i.comment));
                ++drawn;
                continue;
            }
            const auto cls = clipClass(i, lane.track->kind);
            if (width < mergeBelow)
            {
                if (run && run->cls == cls && left - run->end < mergeBelow)
                {
                    run->end = left + width;
                    ++run->count;
                }
                else
                {
                    flush();
                    run = Run{ left, left + width, cls, 1 };
                }
                if (run->end - run->start >= mergeBelow)
                {
                    flush();
                }
                continue;
            }
            flush();
            const auto label = itemLabel(i);
            const auto effects = effectNames(i);
            auto title = label;
            if (!effects.empty())
            {
                title += " (" + effects + ")";
            }
            if (cls == "missing")
            {
                title += " (source not in this file)";
            }
            out_ += std::format(R"(<div class="c {}" style="left:{};width:{}" title="{}">{}{}</div>)", cls, percent(left), percent(width), escapeHtml(title), effects.empty() || cls == "fx" ? "" : "<i>fx</i>", escapeHtml(label));
            ++drawn;
        }
        flush();
    }

    void clipList(const std::vector<Lane>& lanes, const Clock& clock)
    {
        struct Row
        {
            double start;
            double end;
            const Lane* lane;
            const Item* item;
        };
        std::vector<Row> rows;
        for (const auto& lane : lanes)
        {
            if (lane.track->kind != TrackKind::picture && lane.track->kind != TrackKind::sound)
            {
                continue;
            }
            for (const auto& i : lane.track->items)
            {
                if (i.source && i.hasLength && i.length > 0)
                {
                    rows.push_back({ static_cast<double>(i.start) * lane.scale, static_cast<double>(i.start + i.length) * lane.scale, &lane, &i });
                }
            }
        }
        std::ranges::stable_sort(rows, {}, &Row::start);
        out_ += "<h2>Clips</h2>";
        if (rows.empty())
        {
            out_ += "<p class=\"muted\">No clips.</p>";
            return;
        }
        out_ += "<table><thead><tr><th>#</th><th>Track</th><th>Record in</th><th>Record out</th><th>Duration</th><th>Clip</th><th>Effects</th></tr></thead><tbody>";
        const auto shown = std::min(rows.size(), options_.maxClips);
        for (std::size_t n = 0; n < shown; ++n)
        {
            const auto& r = rows[n];
            const auto& i = *r.item;
            if (!i.source)
            {
                continue;
            }
            const auto& source = *i.source;
            std::string clip = escapeHtml(itemLabel(i));
            if (source.original)
            {
                clip += "<span class=\"tag\">original</span>";
            }
            else if (!source.mob)
            {
                clip += "<span class=\"tag err\">not in file</span>";
            }
            else if (source.mobKind == MobKind::composition)
            {
                clip += "<span class=\"tag\">nested</span>";
            }
            out_ += std::format(R"(<tr><td>{}</td><td>{}</td><td class="tc">{}</td><td class="tc">{}</td><td class="tc">{}</td><td>{}</td><td>{}</td></tr>)", n + 1, escapeHtml(r.lane->label), clock.at(r.start), clock.at(r.end), clock.duration(r.end - r.start), clip, escapeHtml(effectNames(i)));
        }
        out_ += "</tbody></table>";
        if (rows.size() > shown)
        {
            out_ += std::format("<p class=\"muted\">and {} more clips</p>", rows.size() - shown);
        }
    }

    void otherCompositions(const std::vector<MobSummary>& mobs, const std::optional<MobSummary>& main)
    {
        std::vector<const MobSummary*> others;
        for (const auto& m : mobs)
        {
            if (m.kind == MobKind::composition && (!main || m.object != main->object))
            {
                others.push_back(&m);
            }
        }
        if (others.empty())
        {
            return;
        }
        out_ += "<h2>Other compositions</h2><table><thead><tr><th>Name</th><th>Tracks</th></tr></thead><tbody>";
        for (const auto* m : others | std::views::take(kMaxCompositionsListed))
        {
            out_ += "<tr><td>" + escapeHtml(m->name.empty() ? "Untitled" : m->name) + (m->topLevel ? "<span class=\"tag\">top level</span>" : "") + "</td><td>" + std::to_string(m->tracks) + "</td></tr>";
        }
        out_ += "</tbody></table>";
        if (others.size() > kMaxCompositionsListed)
        {
            out_ += std::format("<p class=\"muted\">and {} more</p>", others.size() - kMaxCompositionsListed);
        }
    }

    void mobList(const std::vector<MobSummary>& mobs, MobKind kind, std::string_view title)
    {
        std::vector<const MobSummary*> list;
        for (const auto& m : mobs)
        {
            if (m.kind == kind)
            {
                list.push_back(&m);
            }
        }
        if (list.empty())
        {
            return;
        }
        std::ranges::stable_sort(list, {}, [](const MobSummary* m) -> const std::string& { return m->name; });
        out_ += "<h2>" + escapeHtml(title) + "</h2><table><thead><tr><th>Name</th><th>Tracks</th><th>MobID</th></tr></thead><tbody>";
        const auto shown = std::min(list.size(), options_.maxMobs);
        for (const auto* m : list | std::views::take(shown))
        {
            out_ += "<tr><td>" + escapeHtml(m->name.empty() ? "Untitled" : m->name) + "</td><td>" + std::to_string(m->tracks) + "</td><td class=\"tc muted\">" + escapeHtml(m->mobId.toString()) + "</td></tr>";
        }
        out_ += "</tbody></table>";
        if (list.size() > shown)
        {
            out_ += std::format("<p class=\"muted\">and {} more</p>", list.size() - shown);
        }
    }

    const Document& doc_;
    const PreviewOptions& options_;
    timeline::Projector projector_;
    std::string out_;
};

}

auto escapeHtml(std::string_view text) -> std::string
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text)
    {
        switch (c)
        {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&#39;";
                break;
            default:
                out += c;
        }
    }
    return out;
}

auto formatTimecode(std::int64_t frames, std::uint32_t fps, bool drop) -> std::string
{
    if (fps == 0)
    {
        return "--:--:--:--";
    }
    const bool negative = frames < 0;
    auto f = negative ? -frames : frames;
    const std::int64_t nominal = fps;
    if (drop && (nominal == 30 || nominal == 60))
    {
        const std::int64_t dropped = nominal == 30 ? 2 : 4;
        const auto perMinute = nominal * 60 - dropped;
        const auto perTenMinutes = perMinute * 10 + dropped;
        const auto tens = f / perTenMinutes;
        const auto rest = f % perTenMinutes;
        f += dropped * 9 * tens + (rest > dropped ? dropped * ((rest - dropped) / perMinute) : 0);
    }
    const auto ff = f % nominal;
    const auto totalSeconds = f / nominal;
    return std::format("{}{:02}:{:02}:{:02}{}{:02}", negative ? "-" : "", totalSeconds / 3600, totalSeconds / 60 % 60, totalSeconds % 60, drop ? ';' : ':', ff);
}

auto renderPreview(const Document& document, const PreviewOptions& options) -> std::string
{
    return Writer(document, options).render();
}

auto renderErrorPreview(std::string_view fileName, std::string_view message) -> std::string
{
    return std::format(
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><style>:root{{color-scheme:light dark}}body{{font:13px -apple-system,BlinkMacSystemFont,sans-serif;padding:20px}}h1{{font-size:18px}}p{{color:#e5484d}}</style></head>"
        "<body><h1>{}</h1><p>This file could not be previewed: {}</p></body></html>",
        escapeHtml(fileName),
        escapeHtml(message)
    );
}

auto previewFile(const std::filesystem::path& path) -> std::string
{
    const auto name = path.filename().string();
    try
    {
        auto doc = Document::open(path);
        if (!doc)
        {
            return renderErrorPreview(name, doc.error().message);
        }
        PreviewOptions options{ .fileName = name, .fileSize = std::nullopt, .maxClips = 1000, .maxMobs = 200 };
        std::error_code ec;
        if (const auto size = std::filesystem::file_size(path, ec); !ec)
        {
            options.fileSize = size;
        }
        return renderPreview(*doc, options);
    } catch (const std::exception& e)
    {
        return renderErrorPreview(name, e.what());
    } catch (...)
    {
        return renderErrorPreview(name, "unexpected error");
    }
}

}
