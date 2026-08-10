#pragma once

#include <SDL.h>
#include <SDL_ttf.h>

#include <cstddef>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>

namespace spatial_midi {

struct CachedText {
    SDL_Texture* texture = nullptr;
    int width = 0;
    int height = 0;
};

class TextCache {
public:
    TextCache(SDL_Renderer* renderer, TTF_Font* font, SDL_Color foreground,
              std::optional<SDL_Color> background = std::nullopt,
              std::size_t capacity = 128);
    ~TextCache();

    TextCache(const TextCache&) = delete;
    TextCache& operator=(const TextCache&) = delete;

    [[nodiscard]] CachedText get(const std::string& text);
    void clear();

private:
    struct Entry {
        SDL_Texture* texture = nullptr;
        int width = 0;
        int height = 0;
        std::list<std::string>::iterator recency;
    };

    void touch(std::unordered_map<std::string, Entry>::iterator it);
    void evict_if_needed();

    SDL_Renderer* renderer_ = nullptr;
    TTF_Font* font_ = nullptr;
    SDL_Color foreground_{};
    std::optional<SDL_Color> background_;
    std::size_t capacity_ = 128;
    std::list<std::string> recency_;
    std::unordered_map<std::string, Entry> entries_;
};

}
