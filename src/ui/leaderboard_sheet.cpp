#include <beacon/ui/leaderboard.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <format>

namespace beacon {
namespace {

// RFC 4180 rows: quoted fields may contain commas, doubled quotes and line breaks.
std::vector<std::vector<std::string>> parse_csv(std::string_view text) {
    std::vector<std::vector<std::string>> rows(1);
    std::string field;
    bool quoted = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (c == '"' && i + 1 < text.size() && text[i + 1] == '"')
                field += text[++i];
            else if (c == '"')
                quoted = false;
            else
                field += c;
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',' || c == '\n') {
            rows.back().push_back(std::move(field));
            field.clear();
            if (c == '\n')
                rows.emplace_back();
        } else if (c != '\r') {
            field += c;
        }
    }
    rows.back().push_back(std::move(field));
    return rows;
}

// "21 Jan 2025" as "2025-01-21". Some sheets separate the parts with no-break spaces.
std::string iso_date(std::string text) {
    for (std::size_t at = 0; (at = text.find("\xC2\xA0", at)) != std::string::npos;)
        text.replace(at, 2, " ");
    constexpr std::array<std::string_view, 12> months{"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int day = 0;
    int year = 0;
    char month[4] = {};
    if (std::sscanf(text.c_str(), "%d %3s %d", &day, month, &year) != 3)
        return {};
    const auto found = std::ranges::find(months, std::string_view(month));
    if (found == months.end() || day < 1 || day > 31)
        return {};
    return std::format("{:04}-{:02}-{:02}", year, (found - months.begin()) + 1, day);
}

}  // namespace

LeaderboardBoards parse_leaderboard(const std::string_view csv) {
    const auto rows = parse_csv(csv);
    LeaderboardBoards boards;
    if (rows.size() < 2)
        return boards;
    const auto& header = rows[1];
    // Each board runs from its "#" column to the next, its columns found by name.
    for (std::size_t start = 0; start < header.size(); ++start) {
        if (header[start] != "#")
            continue;
        std::size_t end = start + 1;
        while (end < header.size() && header[end] != "#")
            ++end;
        const auto column = [&](const std::string_view name) {
            const auto found = std::find(header.begin() + static_cast<std::ptrdiff_t>(start),
                                         header.begin() + static_cast<std::ptrdiff_t>(end), name);
            return static_cast<std::size_t>(found - header.begin());
        };
        const std::size_t date = column("Date");
        const std::size_t runner = column("Runner");
        const std::size_t igt = column("IGT");
        const std::size_t link = column("Link");
        const std::size_t status = column("Status");
        if (runner == end || igt == end)
            continue;
        const auto cell = [&](const std::vector<std::string>& cells, const std::size_t index) {
            return index < end && index < cells.size() ? std::string_view(cells[index]) : std::string_view{};
        };
        LeaderboardBoard board{.title = start < rows[0].size() ? rows[0][start] : std::string{}};
        for (std::size_t row = 2; row < rows.size(); ++row) {
            const auto& cells = rows[row];
            if (cell(cells, runner).empty() || cell(cells, status) == "Rejected")
                continue;
            const auto address = cell(cells, link);
            board.runs.push_back({.rank = std::atoi(std::string(cell(cells, start)).c_str()),
                                  .runner = std::string(cell(cells, runner)),
                                  .igt = std::string(cell(cells, igt)),
                                  .date = iso_date(std::string(cell(cells, date))),
                                  .link = address.starts_with("https://") || address.starts_with("http://")
                                              ? std::string(address)
                                              : std::string{}});
        }
        boards.push_back(std::move(board));
    }
    return boards;
}

}  // namespace beacon
