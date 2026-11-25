#include "core/backend.h"
#include "core/config.h"
#include "core/style.h"
#include "helpers/guards.h"
#include "helpers/satdump_guards.h"
#include "imgui/imgui_test_helpers.h"
#include "satdump_vars.h"
#include <algorithm>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

// The theme colour parser is not declared in style.h but is exported by satdump_core.
namespace style
{
    void hexToImVec4(std::string color_hex, ImVec4 *this_color);
}

using Catch::Approx;

TEST_CASE("hexToImVec4 parses RRGGBBAA colours", "[core][style]")
{
    ImVec4 c(0, 0, 0, 0);
    style::hexToImVec4("FF8000C0", &c);
    CHECK(c.x == Approx(1.0f));
    CHECK(c.y == Approx(128.0f / 255.0f));
    CHECK(c.z == Approx(0.0f));
    CHECK(c.w == Approx(192.0f / 255.0f));

    style::hexToImVec4("00000000", &c);
    CHECK(c.x == 0.0f);
    CHECK(c.w == 0.0f);
}

TEST_CASE("hexToImVec4 ignores non hex characters and accepts lower case", "[core][style]")
{
    ImVec4 c(0, 0, 0, 0);
    style::hexToImVec4("#ff00ff80", &c);
    CHECK(c.x == Approx(1.0f));
    CHECK(c.y == Approx(0.0f));
    CHECK(c.z == Approx(1.0f));
    CHECK(c.w == Approx(128.0f / 255.0f));

    style::hexToImVec4(" 0x: 10-20-30-40 ", &c);
    // "0x" contributes a leading 0 digit, leaving 9 digits, so this one is rejected
    CHECK(c.x == Approx(1.0f));

    style::hexToImVec4(" #10-20-30-40 ", &c);
    CHECK(c.x == Approx(16.0f / 255.0f));
    CHECK(c.y == Approx(32.0f / 255.0f));
    CHECK(c.z == Approx(48.0f / 255.0f));
    CHECK(c.w == Approx(64.0f / 255.0f));
}

TEST_CASE("hexToImVec4 leaves the colour untouched on invalid input", "[core][style]")
{
    ImVec4 c(0.1f, 0.2f, 0.3f, 0.4f);
    style::hexToImVec4("FFFFFF", &c); // RGB without alpha
    style::hexToImVec4("", &c);
    style::hexToImVec4("FFFFFFFFFF", &c);
    style::hexToImVec4("not a colour", &c);
    CHECK(c.x == 0.1f);
    CHECK(c.y == 0.2f);
    CHECK(c.z == 0.3f);
    CHECK(c.w == 0.4f);
}

TEST_CASE("style::Theme default font settings", "[core][style]")
{
    style::Theme t;
    CHECK(t.font == "Roboto-Medium");
    CHECK(t.font_size == 16.0f);
}

// The tests below run setStyle() and setFonts() in a headless ImGui context, with the themes and
// fonts of the repository's resources

namespace
{
    // Selects a theme and the repository's resources, and restores the globals setStyle() sets
    struct StyleEnv
    {
        test_helpers::ConfigGuard cfg_guard;
        test_helpers::ValueGuard<std::string> respath_guard{satdump::RESPATH, std::string(SATDUMP_TEST_SOURCE_DIR) + "/"};
        test_helpers::ValueGuard<float> device_scale_guard{backend::device_scale, 1.0f};
        test_helpers::ValueGuard<float> ui_scale_guard{ui_scale};
        test_helpers::ValueGuard<style::Theme> theme_guard{style::theme};

        explicit StyleEnv(const std::string &theme, float dpi_scaling = 1.0f)
        {
            satdump::satdump_cfg.main_cfg["user_interface"]["theme"]["value"] = theme;
            satdump::satdump_cfg.main_cfg["user_interface"]["manual_dpi_scaling"]["value"] = dpi_scaling;
        }
    };

    nlohmann::json loadTheme(const std::string &name)
    {
        std::ifstream f(std::string(SATDUMP_TEST_SOURCE_DIR) + "/resources/themes/" + name + ".json");
        return nlohmann::json::parse(f, nullptr, true, true); // AntiSlop.json has comments
    }

    // Index of a theme colour name. Themes may use the names ImGui had before 1.90.9 and 1.91.4.
    int imguiColorIndex(const std::string &name)
    {
        const std::map<std::string, int> legacy = {
            {"TabActive", ImGuiCol_TabSelected},
            {"TabUnfocused", ImGuiCol_TabDimmed},
            {"TabUnfocusedActive", ImGuiCol_TabDimmedSelected},
            {"NavHighlight", ImGuiCol_NavCursor},
        };
        if (legacy.count(name))
            return legacy.at(name);
        for (int i = 0; i < ImGuiCol_COUNT; i++)
            if (name == ImGui::GetStyleColorName(i))
                return i;
        return -1;
    }

    ImVec4 hexColor(const std::string &hex)
    {
        ImVec4 c(-1, -1, -1, -1);
        style::hexToImVec4(hex, &c);
        return c;
    }

    // Largest difference between the components of two colours
    float colorDiff(ImVec4 a, ImVec4 b) { return std::max(std::max(std::fabs(a.x - b.x), std::fabs(a.y - b.y)), std::max(std::fabs(a.z - b.z), std::fabs(a.w - b.w))); }

    // Theme colours that are not RRGGBBAA colours (#413)
    bool isKnownInvalidColor(const std::string &theme, const std::string &section, const std::string &key)
    {
        return theme == "HiContrast-Light" && section == "ImGuiColors" && (key == "Button" || key == "ButtonHovered");
    }

    std::vector<std::string> shippedThemes()
    {
        std::vector<std::string> themes;
        for (auto &e : std::filesystem::directory_iterator(std::string(SATDUMP_TEST_SOURCE_DIR) + "/resources/themes"))
            if (e.path().extension() == ".json")
                themes.push_back(e.path().stem().string());
        std::sort(themes.begin(), themes.end());
        return themes;
    }
} // namespace

TEST_CASE("style::setStyle applies the colours and sizes of each shipped theme", "[core][style]")
{
    auto themes = shippedThemes();
    REQUIRE(themes.size() >= 5);

    for (const std::string &name : themes)
    {
        CAPTURE(name);
        StyleEnv env(name);
        imgui_test::HeadlessContext ctx;
        style::setStyle();
        nlohmann::json theme = loadTheme(name);

        const ImGuiStyle &st = ImGui::GetStyle();
        int checked = 0;
        for (auto &c : theme["ImGuiColors"].items())
        {
            CAPTURE(c.key());
            int idx = imguiColorIndex(c.key());
            CHECK(idx >= 0);
            // The known invalid colours are ignored, see the [!shouldfail] test below
            if (idx >= 0 && !isKnownInvalidColor(name, "ImGuiColors", c.key()))
                CHECK(colorDiff(st.Colors[idx], hexColor(c.value())) < 1e-6f);
            checked++;
        }
        CHECK(checked > 0);

        for (auto &s : theme["ImGuiStyle"].items())
        {
            CAPTURE(s.key());
            if (s.key() == "WindowRounding")
                CHECK(st.WindowRounding == Approx(s.value().get<float>()));
            else if (s.key() == "FrameRounding")
                CHECK(st.FrameRounding == Approx(s.value().get<float>()));
            else if (s.key() == "TabRounding")
                CHECK(st.TabRounding == Approx(s.value().get<float>()));
        }

        for (auto &c : theme["SatDumpColors"].items())
        {
            CAPTURE(c.key());
            if (c.key() == "red")
                CHECK(colorDiff(style::theme.red.Value, hexColor(c.value())) < 1e-6f);
            else if (c.key() == "plot_bg")
                CHECK(colorDiff(style::theme.plot_bg.Value, hexColor(c.value())) < 1e-6f);
        }

        CHECK(style::theme.font == theme.value("font", std::string("Roboto-Medium")));
        CHECK(style::theme.font_size == theme.value("font_size", 16.0f));

        // Without ImNodesColors, the imnodes style is the dark imnodes style
        ImNodesStyle dark;
        ImNodes::StyleColorsDark(&dark);
        if (!theme.contains("ImNodesColors"))
            CHECK(style::theme.imnodes_style.Colors[ImNodesCol_GridBackground] == dark.Colors[ImNodesCol_GridBackground]);
    }
}

TEST_CASE("style::setStyle scales the theme sizes by the UI scale", "[core][style]")
{
    StyleEnv env("Dark", 2.0f);
    imgui_test::HeadlessContext ctx;
    style::setStyle();
    CHECK(ui_scale == 2.0f);
    // Dark.json sets WindowRounding to 2, and ScaleAllSizes() doubles it
    CHECK(ImGui::GetStyle().WindowRounding == Approx(4.0f));
    CHECK(ImGui::GetStyle().WindowPadding.x == Approx(ImGuiStyle().WindowPadding.x * 2.0f));
}

TEST_CASE("style::setStyle uses the light or dark base colours of the theme", "[core][style]")
{
    // Light.json overrides only a few colours, so most come from ImGui::StyleColorsLight()
    ImVec4 light_bg, dark_bg;
    {
        imgui_test::HeadlessContext ref;
        ImGui::StyleColorsLight();
        light_bg = ImGui::GetStyle().Colors[ImGuiCol_MenuBarBg];
        ImGui::StyleColorsDark();
        dark_bg = ImGui::GetStyle().Colors[ImGuiCol_MenuBarBg];
    }
    REQUIRE(colorDiff(light_bg, dark_bg) > 0.1f);
    REQUIRE_FALSE(loadTheme("Light")["ImGuiColors"].contains("MenuBarBg"));

    StyleEnv env("Light");
    imgui_test::HeadlessContext ctx;
    style::setStyle();
    CHECK(colorDiff(ImGui::GetStyle().Colors[ImGuiCol_MenuBarBg], light_bg) < 1e-6f);
}

TEST_CASE("Every colour of the shipped themes is a RRGGBBAA colour", "[core][style]")
{
    int checked = 0;
    for (const std::string &name : shippedThemes())
    {
        CAPTURE(name);
        nlohmann::json theme = loadTheme(name);
        for (const char *section : {"ImGuiColors", "ImNodesColors", "SatDumpColors"})
            for (auto &c : theme[section].items())
            {
                // See the [!shouldfail] test below
                if (isKnownInvalidColor(name, section, c.key()))
                    continue;
                CAPTURE(section, c.key());
                CHECK(hexColor(c.value()).x >= 0);
                checked++;
            }
    }
    CHECK(checked > 100);
}

// Bug: resources/themes/HiContrast-Light.json:34-35 has "Button": "#6DB4FF" and
// "ButtonHovered": "#99ACBFF", which are not RRGGBBAA colours, so hexToImVec4() ignores them (#413)
TEST_CASE("The Button colours of HiContrast-Light are RRGGBBAA colours", "[core][style][!shouldfail]")
{
    nlohmann::json theme = loadTheme("HiContrast-Light");
    for (const char *key : {"Button", "ButtonHovered"})
    {
        CAPTURE(key);
        REQUIRE(isKnownInvalidColor("HiContrast-Light", "ImGuiColors", key));
        CHECK(hexColor(theme["ImGuiColors"][key]).x >= 0);
    }
}

TEST_CASE("style::beginDisabled and endDisabled disable the widgets in between", "[core][style]")
{
    imgui_test::HeadlessContext ctx;
    int clicks = 0;
    ImVec2 center;
    bool disabled = true;
    ImVec4 text_color;
    auto draw = [&]()
    {
        if (disabled)
            style::beginDisabled();
        text_color = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        if (ImGui::Button("Button"))
            clicks++;
        center = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) * 0.5f;
        if (disabled)
            style::endDisabled();
    };
    ctx.frame(draw);
    CHECK(colorDiff(text_color, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)) < 1e-6f);
    ctx.click(center, draw);
    CHECK(clicks == 0);

    disabled = false;
    ctx.click(center, draw);
    CHECK(clicks == 1);
    CHECK(colorDiff(text_color, ImGui::GetStyleColorVec4(ImGuiCol_Text)) < 1e-6f);
}

TEST_CASE("style::setFonts loads the theme font, the icons and the big font", "[core][style]")
{
    StyleEnv env("Dark");
    test_helpers::ValueGuard<ImFont *> base_guard(style::baseFont);
    test_helpers::ValueGuard<ImFont *> big_guard(style::bigFont);
    test_helpers::ValueGuard<std::string> i18n_guard(style::i18n_extraFont, "");
    int rebuilds = 0;
    test_helpers::ValueGuard<std::function<void()>> rebuild_guard(backend::rebuildFonts, [&]() { rebuilds++; });

    imgui_test::HeadlessContext ctx;
    style::setFonts(1.5f);
    CHECK(rebuilds == 1);
    REQUIRE(style::baseFont != nullptr);
    REQUIRE(style::bigFont != nullptr);
    CHECK(style::baseFont != style::bigFont);

    // The icon fonts are merged into the base font
    CHECK(ImGui::GetIO().Fonts->Fonts.Size == 2);
    CHECK(style::baseFont->LegacySize == Approx(16.0f * 1.5f));
    CHECK(style::bigFont->LegacySize == Approx(45.0f * 1.5f));
    CHECK(style::baseFont->IsGlyphInFont('A'));
    CHECK(style::baseFont->IsGlyphInFont(0xf00c)); // An icon of font.ttf
    CHECK_FALSE(style::bigFont->IsGlyphInFont(0xf00c));

    ctx.frames(2,
               []()
               {
                   ImGui::PushFont(style::baseFont, 0.0f);
                   ImGui::Text("Base \xef\x80\x8c"); // U+F00C
                   ImGui::PopFont();
                   ImGui::PushFont(style::bigFont, 0.0f);
                   ImGui::Text("Big");
                   ImGui::PopFont();
               });
    CHECK(ctx.lastVertexCount() > 0);
}
