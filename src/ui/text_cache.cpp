#include "spatial_midi/ui/text_cache.hpp"

#include <stdexcept>

namespace spatial_midi {
    TextCache::TextCache(SDL_Renderer *renderer, TTF_Font *font, SDL_Color foreground,
                         std::optional<SDL_Color> background, std::size_t capacity)
        : renderer_(renderer), font_(font), foreground_(foreground), background_(background), capacity_(capacity) {
        if (!renderer_ || !font_ || capacity_ == 0) {
            throw std::invalid_argument("TextCache requires renderer, font, and non-zero capacity");
        }
    }

    TextCache::~TextCache() {
        clear();
    }

    CachedText TextCache::get(const std::string &text) {
        if (const auto found = entries_.find(text); found != entries_.end()) {
            touch(found);
            return {found->second.texture, found->second.width, found->second.height,};
        }

        SDL_Surface *surface = background_
                                   ? TTF_RenderUTF8_Shaded(font_, text.c_str(), foreground_, *background_)
                                   : TTF_RenderUTF8_Blended(font_, text.c_str(), foreground_);
        if (!surface) {
            throw std::runtime_error(std::string("TTF render: ") + TTF_GetError());
        }

        SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer_, surface);
        const int width = surface->w;
        const int height = surface->h;
        SDL_FreeSurface(surface);

        if (!texture) {
            throw std::runtime_error(std::string("SDL_CreateTextureFromSurface: ") + SDL_GetError());
        }

        recency_.push_front(text);
        entries_.emplace(text, Entry{texture, width, height, recency_.begin()});
        evict_if_needed();
        return {texture, width, height};
    }

    void TextCache::clear() {
        for (auto &[key, entry]: entries_) {
            (void) key;
            SDL_DestroyTexture(entry.texture);
        }
        entries_.clear();
        recency_.clear();
    }

    void TextCache::touch(std::unordered_map<std::string, Entry>::iterator it) {
        recency_.splice(recency_.begin(), recency_, it->second.recency);
        it->second.recency = recency_.begin();
    }

    void TextCache::evict_if_needed() {
        while (entries_.size() > capacity_) {
            if (const auto it = entries_.find(recency_.back()); it != entries_.end()) {
                SDL_DestroyTexture(it->second.texture);
                entries_.erase(it);
            }
            recency_.pop_back();
        }
    }
}
