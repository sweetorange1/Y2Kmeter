#include "source/ui/toys/ToyKit.h"

// ==========================================================
// ToyTheme —— 语义角色表（E.3.1）
//   玩法只写角色（ToyRole），由当前主题决定用哪个 emoji。
//   实时映射 PinkXP 当前主题，无缓存（PinkXP 色值是 inline 全局，
//   主题切换时已被 applyTheme 更新）。
// ==========================================================

namespace
{
    struct RoleSet
    {
        EmojiId reward, burst, orb, foe, cheer, pin;
    };

    // E.3.1 草案表（各主题只覆盖角色，其余用默认值）
    RoleSet roleSetFor (PinkXP::ThemeId id)
    {
        using T = PinkXP::ThemeId;
        switch (id)
        {
            case T::blackPink:    return { EmojiId::Gem,         EmojiId::Boom,        EmojiId::Cherry,      EmojiId::SpaceInvader, EmojiId::Butterfly, EmojiId::BlackHeart };
            case T::bubblegum:    return { EmojiId::Candy,       EmojiId::Blossom,     EmojiId::Strawberry,  EmojiId::SpaceInvader, EmojiId::Ribbon,    EmojiId::Lollipop };
            case T::starlight:    return { EmojiId::GlowStar,    EmojiId::Comet,       EmojiId::Moon,        EmojiId::Alien,        EmojiId::Sparkles,  EmojiId::Star };
            case T::cyberLilac:   return { EmojiId::Cd,          EmojiId::Bolt,        EmojiId::CrystalBall, EmojiId::SpaceInvader, EmojiId::Headphone, EmojiId::Joystick };
            case T::tangerinePop: return { EmojiId::Cocktail,    EmojiId::Sun,         EmojiId::Orange,      EmojiId::SpaceInvader, EmojiId::IceCream,  EmojiId::Orange };
            case T::aquaPearl:    return { EmojiId::Shell,       EmojiId::Drop,        EmojiId::Drop,        EmojiId::Crab,         EmojiId::Dolphin,   EmojiId::OceanWave };
            case T::matchaSoda:   return { EmojiId::CupWithStraw,EmojiId::Leaf,        EmojiId::GreenApple,  EmojiId::Mushroom,     EmojiId::Clover,    EmojiId::Herb };
            case T::jungle:       return { EmojiId::Butterfly,   EmojiId::Leaf,        EmojiId::Mushroom,    EmojiId::Lizard,       EmojiId::Frog,      EmojiId::PalmTree };
            case T::winXP:        return { EmojiId::Floppy,      EmojiId::Boom,        EmojiId::Cd,          EmojiId::Mouse,        EmojiId::Laptop,    EmojiId::Folder };
            case T::crimsonNoir:  return { EmojiId::Rose,        EmojiId::Bat,         EmojiId::BlackHeart,  EmojiId::Spider,       EmojiId::Bat,       EmojiId::WiltedRose };
            case T::voidGrey:
            case T::paperGrey:    return { EmojiId::Check,       EmojiId::Plus,        EmojiId::BlackSquare, EmojiId::FrameSquare,  EmojiId::ButtonSquare, EmojiId::WhiteSquare };
            default:              break;
        }
        return { EmojiId::MoneyBag, EmojiId::Boom, EmojiId::RedCircle, EmojiId::SpaceInvader, EmojiId::RaisedHand, EmojiId::Flag };
    }

    const RoleSet& currentRoleSet()
    {
        static RoleSet cached;
        static int cachedKey = -1;
        const int key = (int) PinkXP::getCurrentTheme().id;
        if (key != cachedKey)
        {
            cached = roleSetFor ((PinkXP::ThemeId) key);
            cachedKey = key;
        }
        return cached;
    }
}

EmojiId toyRoleEmoji (ToyRole role)
{
    const auto& s = currentRoleSet();
    switch (role)
    {
        case ToyRole::Reward: return s.reward;
        case ToyRole::Burst:  return s.burst;
        case ToyRole::Orb:    return s.orb;
        case ToyRole::Foe:    return s.foe;
        case ToyRole::Cheer:  return s.cheer;
        case ToyRole::Pin:    return s.pin;
        case ToyRole::Alert:  return EmojiId::Warning;   // 固定（语义不能变）
        case ToyRole::Clear:  return EmojiId::Check;
        case ToyRole::Flame:  return EmojiId::Fire;
        default:              return EmojiId::Sparkles;
    }
}
