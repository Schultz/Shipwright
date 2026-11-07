#ifdef ENABLE_RETROACHIEVEMENTS

#include "RAPopup.h"
#include "RAHttp.h"

#include <algorithm>
#include <deque>
#include <memory>
#include <unordered_map>
#include <imgui.h>
#include <spdlog/spdlog.h>
#include <stb_image.h>
#include <ship/Context.h>
#include <ship/window/Window.h>
#include <ship/window/gui/GuiWindow.h>
#include <ship/window/gui/resource/GuiTexture.h>
#include <fast/Fast3dGui.h>
#include <libultraship/bridge/consolevariablebridge.h>

extern "C" {
#include "z64.h"
#include "functions.h"
}

#define CVAR_RA(var) CVAR_REMOTE("RetroAchievements." var)

namespace {

// ---- Badge cache ----

enum class BadgeState { Downloading, Downloaded, Ready, Failed };

struct Badge {
    BadgeState state = BadgeState::Downloading;
    std::string png;
    std::string textureName;
};

std::unordered_map<std::string, Badge> sBadges;
uint32_t sNextTextureId = 0;

std::shared_ptr<Fast::Fast3dGui> GetGui() {
    return std::dynamic_pointer_cast<Fast::Fast3dGui>(Ship::Context::GetRawInstance()->GetWindow()->GetGui());
}

// Decodes and uploads a downloaded badge. Runs from the GUI draw, the same place LUS windows
// load their textures, so the upload doesn't land in the middle of the game's frame.
void UploadBadge(Badge& badge) {
    int width = 0, height = 0, channels = 0;
    uint8_t* pixels =
        stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(badge.png.data()), static_cast<int>(badge.png.size()),
                              &width, &height, &channels, STBI_rgb_alpha);
    badge.png.clear();
    badge.png.shrink_to_fit();
    if (pixels == nullptr) {
        badge.state = BadgeState::Failed;
        return;
    }

    badge.textureName = "RA_BADGE_" + std::to_string(sNextTextureId++);

    // ~IResource logs InitData->Path, so the resource needs init data even though it
    // never came from an archive (the default constructor leaves it null and crashes)
    auto initData = std::make_shared<Ship::ResourceInitData>();
    initData->Type = RESOURCE_TYPE_GUI_TEXTURE;
    initData->Path = badge.textureName;

    auto texture = std::make_shared<Ship::GuiTexture>(initData);
    texture->Data = pixels; // freed with stbi_image_free by ~GuiTexture after the upload
    texture->DataSize = static_cast<size_t>(width) * height * 4;
    texture->Metadata.Width = width;
    texture->Metadata.Height = height;

    GetGui()->LoadTextureFromResource(badge.textureName, texture);
    badge.state = BadgeState::Ready;
}

ImTextureID GetBadgeTexture(const std::string& url) {
    auto it = sBadges.find(url);
    if (it == sBadges.end()) {
        return nullptr;
    }
    Badge& badge = it->second;
    if (badge.state == BadgeState::Downloaded) {
        UploadBadge(badge);
    }
    return badge.state == BadgeState::Ready ? GetGui()->GetTextureByName(badge.textureName) : nullptr;
}

// ---- Popup queue and drawing ----

constexpr float SLIDE_SECONDS = 0.35f;
constexpr float HOLD_SECONDS = 5.0f;

struct ActivePopup {
    RAPopup::Popup popup;
    float elapsed = 0.0f;
    bool started = false;
};

std::deque<ActivePopup> sQueue;

float EaseOutCubic(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

// How far the banner has slid in (0 = off screen, 1 = fully shown)
float SlideAmount(float elapsed) {
    if (elapsed < SLIDE_SECONDS) {
        return EaseOutCubic(elapsed / SLIDE_SECONDS);
    }
    if (elapsed < SLIDE_SECONDS + HOLD_SECONDS) {
        return 1.0f;
    }
    return 1.0f - EaseOutCubic((elapsed - SLIDE_SECONDS - HOLD_SECONDS) / SLIDE_SECONDS);
}

void DrawPopup(ActivePopup& active) {
    const RAPopup::Popup& p = active.popup;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* draw = ImGui::GetForegroundDrawList(vp);
    ImFont* font = ImGui::GetFont();

    // Scale with the window so the banner reads the same at 720p and 4K
    const float scale = std::max(1.0f, vp->Size.y / 720.0f);
    const float margin = 24.0f * scale;
    const float pad = 12.0f * scale;
    const float badgeSize = 64.0f * scale;
    const float width = 440.0f * scale;
    const float height = badgeSize + pad * 2;
    const float rounding = 10.0f * scale;

    const float slide = SlideAmount(active.elapsed);
    const float x = vp->Pos.x + vp->Size.x - margin - width + (1.0f - slide) * (width + margin);
    const float y = vp->Pos.y + margin;
    const ImVec2 min(x, y);
    const ImVec2 max(x + width, y + height);
    const float alpha = std::clamp(slide * 1.5f, 0.0f, 1.0f);
    auto color = [alpha](int r, int g, int b, int a) { return IM_COL32(r, g, b, static_cast<int>(a * alpha)); };

    const ImU32 accent = p.gold ? color(232, 186, 80, 255) : color(170, 178, 196, 255);

    // Panel: drop shadow, dark gradient body, accent edge
    draw->AddRectFilled(ImVec2(min.x + 4 * scale, min.y + 5 * scale), ImVec2(max.x + 4 * scale, max.y + 5 * scale),
                        color(0, 0, 0, 110), rounding);
    draw->AddRectFilledMultiColor(min, max, color(34, 36, 46, 240), color(22, 23, 30, 240), color(16, 17, 22, 240),
                                  color(26, 27, 36, 240));
    draw->AddRect(min, max, color(255, 255, 255, 28), rounding, 0, 1.0f * scale);
    draw->AddRectFilled(min, ImVec2(min.x + 4 * scale, max.y), accent, rounding, ImDrawFlags_RoundCornersLeft);

    // Badge, or a placeholder disc while it downloads
    const ImVec2 badgeMin(min.x + pad + 4 * scale, min.y + pad);
    const ImVec2 badgeMax(badgeMin.x + badgeSize, badgeMin.y + badgeSize);
    ImTextureID texture = p.badgeUrl.empty() ? nullptr : GetBadgeTexture(p.badgeUrl);
    if (texture != nullptr) {
        draw->AddImageRounded(texture, badgeMin, badgeMax, ImVec2(0, 0), ImVec2(1, 1), color(255, 255, 255, 255),
                              6.0f * scale);
    } else {
        draw->AddRectFilled(badgeMin, badgeMax, color(52, 54, 66, 255), 6.0f * scale);
        draw->AddCircleFilled(ImVec2((badgeMin.x + badgeMax.x) / 2, (badgeMin.y + badgeMax.y) / 2), badgeSize * 0.22f,
                              accent);
    }
    draw->AddRect(badgeMin, badgeMax, accent, 6.0f * scale, 0, 1.5f * scale);

    // Text column
    const float textX = badgeMax.x + pad;
    const float textRight = max.x - pad;
    const float base = ImGui::GetFontSize() * scale;
    float textY = min.y + pad - 2 * scale;

    const ImVec4 clip(textX, min.y, textRight, max.y);
    draw->AddText(font, base * 0.85f, ImVec2(textX, textY), accent, p.headline.c_str(), nullptr, 0.0f, &clip);
    if (p.points > 0) {
        const std::string points = std::to_string(p.points) + " pts";
        const ImVec2 size = font->CalcTextSizeA(base * 0.85f, FLT_MAX, 0.0f, points.c_str());
        draw->AddText(font, base * 0.85f, ImVec2(textRight - size.x, textY), accent, points.c_str());
    }
    textY += base * 0.95f;
    draw->AddText(font, base * 1.15f, ImVec2(textX, textY), color(255, 255, 255, 255), p.title.c_str(), nullptr, 0.0f,
                  &clip);
    textY += base * 1.25f;
    draw->AddText(font, base * 0.8f, ImVec2(textX, textY), color(176, 180, 192, 255), p.description.c_str(), nullptr,
                  textRight - textX, &clip);
}

class PopupWindow final : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;

    void InitElement() override {
    }
    void DrawElement() override {
    }
    void UpdateElement() override {
    }

    void Draw() override {
        if (sQueue.empty()) {
            return;
        }
        ActivePopup& active = sQueue.front();
        if (!active.started) {
            active.started = true;
            if (CVarGetInteger(CVAR_RA("Sound"), 1)) {
                Sfx_PlaySfxCentered(NA_SE_SY_CORRECT_CHIME);
            }
        }
        DrawPopup(active);
        active.elapsed += ImGui::GetIO().DeltaTime;
        if (active.elapsed >= SLIDE_SECONDS * 2 + HOLD_SECONDS) {
            sQueue.pop_front();
        }
    }
};

} // namespace

namespace RAPopup {

void Init() {
    auto gui = GetGui();
    auto window = std::make_shared<PopupWindow>(CVAR_WINDOW("RetroAchievementsPopup"), "RetroAchievements Popup");
    gui->AddGuiWindow(window);
    window->Show();
}

void PrefetchBadge(const std::string& url) {
    if (url.empty() || sBadges.count(url) != 0) {
        return;
    }
    sBadges[url] = Badge{};
    RAHttp::Request(url, "", "", [url](const RAHttp::Response& response) {
        Badge& badge = sBadges[url];
        if (response.statusCode == 200 && !response.body.empty()) {
            badge.png = response.body;
            badge.state = BadgeState::Downloaded;
        } else {
            SPDLOG_WARN("[RetroAchievements] Badge download failed ({}): {}", response.statusCode, url);
            badge.state = BadgeState::Failed;
        }
    });
}

void Show(const Popup& popup) {
    PrefetchBadge(popup.badgeUrl);
    sQueue.push_back({ popup, 0.0f, false });
}

} // namespace RAPopup

#endif // ENABLE_RETROACHIEVEMENTS
