#include "station.h"
#include "companymgr.h"
#include "industrymgr.h"
#include "interop/interop.hpp"
#include "localisation/string_ids.h"
#include "map/tilemgr.h"
#include "messagemgr.h"
#include "objects/airport_object.h"
#include "objects/building_object.h"
#include "objects/cargo_object.h"
#include "objects/industry_object.h"
#include "objects/objectmgr.h"
#include "objects/road_station_object.h"
#include "openloco.h"
#include "ui/WindowManager.h"
#include "viewportmgr.h"
#include <algorithm>
#include <cassert>

using namespace openloco::interop;
using namespace openloco::map;
using namespace openloco::ui;

namespace openloco
{
    constexpr uint8_t min_cargo_rating = 0;
    constexpr uint8_t max_cargo_rating = 200;
    constexpr uint8_t catchmentSize = 4;

    struct CargoSearchState
    {
    private:
        inline static loco_global<uint8_t[map_size], 0x00F00484> _map;
        inline static loco_global<uint32_t, 0x0112C68C> _filter;
        inline static loco_global<uint32_t[max_cargo_stats], 0x0112C690> _score;
        inline static loco_global<uint32_t, 0x0112C710> _producedCargoTypes;
        inline static loco_global<industry_id_t[max_cargo_stats], 0x0112C7D2> _industry;
        inline static loco_global<uint8_t, 0x0112C7F2> _byte_112C7F2;

    public:
        bool mapHas2(const tile_coord_t x, const tile_coord_t y) const
        {
            return (_map[y * map_columns + x] & (1 << 1)) != 0;
        }

        void mapRemove2(const tile_coord_t x, const tile_coord_t y)
        {
            _map[y * map_columns + x] &= ~(1 << 1);
        }

        void setTile(const tile_coord_t x, const tile_coord_t y, const uint8_t flag)
        {
            _map[y * map_columns + x] |= (1 << flag);
        }

        void resetTile(const tile_coord_t x, const tile_coord_t y, const uint8_t flag)
        {
            _map[y * map_columns + x] &= ~(1 << flag);
        }

        void setTileRegion(tile_coord_t x, tile_coord_t y, int16_t xTileCount, int16_t yTileCount, const uint8_t flag)
        {
            auto xStart = x;
            auto xTileStartCount = xTileCount;
            while (yTileCount > 0)
            {
                while (xTileCount > 0)
                {
                    setTile(x, y, flag);
                    x++;
                    xTileCount--;
                }

                x = xStart;
                xTileCount = xTileStartCount;
                y++;
                yTileCount--;
            }
        }

        void resetTileRegion(tile_coord_t x, tile_coord_t y, int16_t xTileCount, int16_t yTileCount, const uint8_t flag)
        {
            auto xStart = x;
            auto xTileStartCount = xTileCount;
            while (yTileCount > 0)
            {
                while (xTileCount > 0)
                {
                    resetTile(x, y, flag);
                    x++;
                    xTileCount--;
                }

                x = xStart;
                xTileCount = xTileStartCount;
                y++;
                yTileCount--;
            }
        }

        uint32_t filter() const
        {
            return _filter;
        }

        void filter(const uint32_t value)
        {
            _filter = value;
        }

        void resetScores()
        {
            std::fill_n(_score.get(), max_cargo_stats, 0);
        }

        uint32_t score(const uint8_t cargo)
        {
            return _score[cargo];
        }

        void addScore(const uint8_t cargo, const int32_t value)
        {
            _score[cargo] += value;
        }

        uint32_t producedCargoTypes() const
        {
            return _producedCargoTypes;
        }

        void resetProducedCargoTypes()
        {
            _producedCargoTypes = 0;
        }

        void addProducedCargoType(const uint8_t cargoId)
        {
            _producedCargoTypes = _producedCargoTypes | (1 << cargoId);
        }

        void byte_112C7F2(const uint8_t value)
        {
            _byte_112C7F2 = value;
        }

        void resetIndustryMap()
        {
            std::fill_n(_industry.get(), max_cargo_stats, industry_id::null);
        }

        industry_id_t getIndustry(const uint8_t cargo) const
        {
            return _industry[cargo];
        }

        void setIndustry(const uint8_t cargo, const industry_id_t id)
        {
            _industry[cargo] = id;
        }
    };

    static void sub_491BF5(const map_pos& pos, const uint8_t flag);
    static station_element* getStationElement(const map_pos3& pos);

    station_id_t station::id() const
    {
        // TODO check if this is stored in station structure
        //      otherwise add it when possible
        static loco_global<station[1024], 0x005E6EDC> _stations;
        auto index = (size_t)(this - _stations);
        if (index > 1024)
        {
            index = station_id::null;
        }
        return (station_id_t)index;
    }

    // 0x0048B23E
    void station::update()
    {
        update_cargo_acceptance();
    }

    // 0x00492640
    void station::update_cargo_acceptance()
    {
        CargoSearchState cargoSearchState;
        uint32_t currentAcceptedCargo = calcAcceptedCargo(cargoSearchState);
        uint32_t originallyAcceptedCargo = 0;
        for (uint32_t cargoId = 0; cargoId < max_cargo_stats; cargoId++)
        {
            auto& cs = cargo_stats[cargoId];
            cs.industry_id = cargoSearchState.getIndustry(cargoId);
            if (cs.is_accepted())
            {
                originallyAcceptedCargo |= (1 << cargoId);
            }

            bool isNowAccepted = (currentAcceptedCargo & (1 << cargoId)) != 0;
            cs.is_accepted(isNowAccepted);
        }

        if (originallyAcceptedCargo != currentAcceptedCargo)
        {
            if (owner == companymgr::get_controlling_id())
            {
                alert_cargo_acceptance_change(originallyAcceptedCargo, currentAcceptedCargo);
            }
            invalidate_window();
        }
    }

    // 0x00492683
    void station::alert_cargo_acceptance_change(uint32_t oldCargoAcc, uint32_t newCargoAcc)
    {
        for (uint32_t cargoId = 0; cargoId < max_cargo_stats; cargoId++)
        {
            bool acceptedBefore = (oldCargoAcc & (1 << cargoId)) != 0;
            bool acceptedNow = (newCargoAcc & (1 << cargoId)) != 0;
            if (acceptedBefore && !acceptedNow)
            {
                messagemgr::post(
                    messageType::cargoNoLongerAccepted,
                    owner,
                    id(),
                    cargoId);
            }
            else if (!acceptedBefore && acceptedNow)
            {
                messagemgr::post(
                    messageType::cargoNowAccepted,
                    owner,
                    id(),
                    cargoId);
            }
        }
    }

    // 0x00491FE0
    // WARNING: this may be called with station (ebp) = -1
    // filter only used if location.x != -1
    uint32_t station::calcAcceptedCargo(CargoSearchState& cargoSearchState, const map_pos& location, const uint32_t filter)
    {
        cargoSearchState.byte_112C7F2(1);
        cargoSearchState.filter(0);

        if (location.x != -1)
        {
            cargoSearchState.filter(filter);
        }

        cargoSearchState.resetIndustryMap();

        setCatchmentDisplay(1);

        if (location.x != -1)
        {
            sub_491BF5(location, 1);
        }

        cargoSearchState.resetScores();
        cargoSearchState.resetProducedCargoTypes();

        if (this != (station*)0xFFFFFFFF)
        {
            for (uint16_t i = 0; i < stationTileSize; i++)
            {
                auto pos = stationTiles[i];
                auto stationElement = getStationElement(pos);

                if (stationElement == nullptr)
                {
                    continue;
                }

                cargoSearchState.byte_112C7F2(0);

                if (stationElement->stationType() == stationType::roadStation)
                {
                    auto obj = objectmgr::get<road_station_object>(stationElement->object_id());

                    if (obj->flags & road_station_flags::passenger)
                    {
                        cargoSearchState.filter(cargoSearchState.filter() | (1 << obj->var_2C));
                    }
                    else if (obj->flags & road_station_flags::freight)
                    {
                        cargoSearchState.filter(cargoSearchState.filter() | ~(1 << obj->var_2C));
                    }
                }
                else
                {
                    cargoSearchState.filter(~0);
                }
            }
        }

        if (cargoSearchState.filter() == 0)
        {
            cargoSearchState.filter(~0);
        }

        for (tile_coord_t ty = 0; ty < map_columns; ty++)
        {
            for (tile_coord_t tx = 0; tx < map_rows; tx++)
            {
                if (cargoSearchState.mapHas2(tx, ty))
                {
                    auto pos = map_pos(tx * tile_size, ty * tile_size);
                    auto tile = tilemgr::get(pos);

                    for (auto& el : tile)
                    {
                        if (el.is_flag_4())
                        {
                            continue;
                        }
                        switch (el.type())
                        {
                            case element_type::industry:
                            {
                                auto industryEl = el.as_industry();
                                auto industry = industryEl->industry();

                                if (industry == nullptr || industry->under_construction != 0xFF)
                                {
                                    break;
                                }
                                auto obj = industry->object();

                                if (obj == nullptr)
                                {
                                    break;
                                }

                                for (auto cargoId : obj->required_cargo_type)
                                {
                                    if (cargoId != 0xFF && (cargoSearchState.filter() & (1 << cargoId)))
                                    {
                                        cargoSearchState.addScore(cargoId, 8);
                                        cargoSearchState.setIndustry(cargoId, industry->id());
                                    }
                                }

                                for (auto cargoId : obj->produced_cargo_type)
                                {
                                    if (cargoId != 0xFF && (cargoSearchState.filter() & (1 << cargoId)))
                                    {
                                        cargoSearchState.addProducedCargoType(cargoId);
                                    }
                                }

                                break;
                            }
                            case element_type::building:
                            {
                                auto buildingEl = el.as_building();

                                if (buildingEl == nullptr || buildingEl->has_40() || !buildingEl->has_station_element())
                                {
                                    break;
                                }

                                auto obj = buildingEl->object();

                                if (obj == nullptr)
                                {
                                    break;
                                }
                                for (int i = 0; i < 2; i++)
                                {
                                    const auto cargoId = obj->producedCargoType[i];
                                    if (cargoId != 0xFF && (cargoSearchState.filter() & (1 << cargoId)))
                                    {
                                        cargoSearchState.addScore(cargoId, obj->var_A6[i]);

                                        if (obj->var_A0[i] != 0)
                                        {
                                            cargoSearchState.addProducedCargoType(cargoId);
                                        }
                                    }
                                }

                                for (int i = 0; i < 2; i++)
                                {
                                    if (obj->var_A4[i] != 0xFF && (cargoSearchState.filter() & (1 << obj->var_A4[i])))
                                    {
                                        cargoSearchState.addScore(obj->var_A4[i], obj->var_A8[i]);
                                    }
                                }

                                // Multi tile buildings should only be counted once so remove the other tiles from the search
                                if (obj->flags & building_object_flags::large_tile)
                                {
                                    // 0x004F9296, 0x4F9298
                                    static const map_pos offsets[4] = { { 0, 0 }, { 0, 32 }, { 32, 32 }, { 32, 0 } };

                                    auto index = buildingEl->multiTileIndex();
                                    tile_coord_t xPos = (pos.x - offsets[index].x) / tile_size;
                                    tile_coord_t yPos = (pos.y - offsets[index].y) / tile_size;

                                    cargoSearchState.mapRemove2(xPos + 0, yPos + 0);
                                    cargoSearchState.mapRemove2(xPos + 0, yPos + 1);
                                    cargoSearchState.mapRemove2(xPos + 1, yPos + 0);
                                    cargoSearchState.mapRemove2(xPos + 1, yPos + 1);
                                }

                                break;
                            }
                            default:
                                continue;
                        }
                    }
                }
            }
        }

        uint32_t acceptedCargos = 0;

        for (uint8_t cargoId = 0; cargoId < max_cargo_stats; cargoId++)
        {
            if (cargoSearchState.score(cargoId) >= 8)
            {
                acceptedCargos |= (1 << cargoId);
            }
        }

        return acceptedCargos;
    }

    static void setStationCatchmentRegion(CargoSearchState& cargoSearchState, TilePos minPos, TilePos maxPos, const uint8_t flags);

    // 0x00491D70
    // catchment flag should not be shifted (1, 2, 3, 4) and NOT (1 << 0, 1 << 1)
    void station::setCatchmentDisplay(const uint8_t catchmentFlag)
    {
        CargoSearchState cargoSearchState;
        cargoSearchState.resetTileRegion(0, 0, map_columns, map_rows, catchmentFlag);

        if (this == (station*)0xFFFFFFFF)
            return;

        if (stationTileSize == 0)
            return;

        for (uint16_t i = 0; i < stationTileSize; i++)
        {
            auto pos = stationTiles[i];
            pos.z &= ~((1 << 1) | (1 << 0));

            auto stationElement = getStationElement(pos);

            if (stationElement == nullptr)
                continue;

            switch (stationElement->stationType())
            {
                case stationType::airport:
                {
                    auto airportObject = objectmgr::get<airport_object>(stationElement->object_id());

                    map_pos minPos, maxPos;
                    minPos.x = airportObject->min_x;
                    minPos.y = airportObject->min_y;
                    maxPos.x = airportObject->max_x;
                    maxPos.y = airportObject->max_y;

                    minPos = rotate2DCoordinate(minPos, stationElement->rotation());
                    maxPos = rotate2DCoordinate(maxPos, stationElement->rotation());

                    minPos.x += pos.x;
                    minPos.y += pos.y;
                    maxPos.x += pos.x;
                    maxPos.y += pos.y;

                    if (minPos.x > maxPos.x)
                    {
                        std::swap(minPos.x, maxPos.x);
                    }

                    if (minPos.y > maxPos.y)
                    {
                        std::swap(minPos.y, maxPos.y);
                    }

                    TilePos tileMinPos(minPos);
                    TilePos tileMaxPos(maxPos);

                    tileMinPos.x -= catchmentSize;
                    tileMinPos.y -= catchmentSize;
                    tileMaxPos.x += catchmentSize;
                    tileMaxPos.y += catchmentSize;

                    setStationCatchmentRegion(cargoSearchState, tileMinPos, tileMaxPos, catchmentFlag);
                }
                break;
                case stationType::docks:
                {
                    TilePos minPos(pos);
                    auto maxPos = minPos;

                    minPos.x -= catchmentSize;
                    minPos.y -= catchmentSize;
                    // Docks are always size 2x2
                    maxPos.x += catchmentSize + 1;
                    maxPos.y += catchmentSize + 1;

                    setStationCatchmentRegion(cargoSearchState, minPos, maxPos, catchmentFlag);
                }
                break;
                default:
                {
                    TilePos minPos(pos);
                    auto maxPos = minPos;

                    minPos.x -= catchmentSize;
                    minPos.y -= catchmentSize;
                    maxPos.x += catchmentSize;
                    maxPos.y += catchmentSize;

                    setStationCatchmentRegion(cargoSearchState, minPos, maxPos, catchmentFlag);
                }
            }
        }
    }

    // 0x0048F7D1
    void station::sub_48F7D1()
    {
        registers regs;
        regs.ebx = id();
        call(0x0048F7D1, regs);
    }

    // 0x00492A98
    void station::getStatusString(const char* buffer)
    {
        char* ptr = (char*)buffer;
        *ptr = '\0';

        for (uint32_t cargoId = 0; cargoId < max_cargo_stats; cargoId++)
        {
            auto& stats = cargo_stats[cargoId];

            if (stats.quantity == 0)
                continue;

            if (*buffer != '\0')
                ptr = stringmgr::format_string(ptr, string_ids::waiting_cargo_separator);

            loco_global<uint32_t, 0x112C826> _common_format_args;
            *_common_format_args = stats.quantity;

            auto cargo = objectmgr::get<cargo_object>(cargoId);
            string_id unit_name = stats.quantity == 1 ? cargo->unit_name_singular : cargo->unit_name_plural;
            ptr = stringmgr::format_string(ptr, unit_name, &*_common_format_args);
        }

        string_id suffix = *buffer == '\0' ? string_ids::nothing_waiting : string_ids::waiting;
        ptr = stringmgr::format_string(ptr, suffix);
    }

    // 0x00492793
    bool station::update_cargo()
    {
        bool atLeastOneGoodRating = false;
        bool quantityUpdated = false;

        var_3B0 = std::min(var_3B0 + 1, 255);
        var_3B1 = std::min(var_3B1 + 1, 255);

        auto& rng = gprng();
        for (uint32_t i = 0; i < max_cargo_stats; i++)
        {
            auto& cargo = cargo_stats[i];
            if (!cargo.empty())
            {
                if (cargo.quantity != 0 && cargo.origin != id())
                {
                    cargo.enroute_age = std::min(cargo.enroute_age + 1, 255);
                }
                cargo.age = std::min(cargo.age + 1, 255);

                auto targetRating = calculate_cargo_rating(cargo);
                // Limit to +/- 2 minimum change
                auto ratingDelta = std::clamp(targetRating - cargo.rating, -2, 2);
                cargo.rating += ratingDelta;

                if (cargo.rating <= 50)
                {
                    // Rating < 25%, decrease cargo
                    if (cargo.quantity >= 400)
                    {
                        cargo.quantity -= rng.randNext(1, 32);
                        quantityUpdated = true;
                    }
                    else if (cargo.quantity >= 200)
                    {
                        cargo.quantity -= rng.randNext(1, 8);
                        quantityUpdated = true;
                    }
                }
                if (cargo.rating >= 100)
                {
                    atLeastOneGoodRating = true;
                }
                if (cargo.rating <= 100 && cargo.quantity != 0)
                {
                    if (cargo.rating <= rng.randNext(0, 127))
                    {
                        cargo.quantity = std::max(0, cargo.quantity - rng.randNext(1, 4));
                        quantityUpdated = true;
                    }
                }
            }
        }

        sub_4929DB();

        auto w = WindowManager::find(WindowType::station, id());
        if (w != nullptr && (w->current_tab == 2 || w->current_tab == 1 || quantityUpdated))
        {
            w->invalidate();
        }

        return atLeastOneGoodRating;
    }

    // 0x004927F6
    int32_t station::calculate_cargo_rating(const station_cargo_stats& cargo) const
    {
        int32_t rating = 0;

        // Bonus if cargo is fresh
        if (cargo.age <= 45)
        {
            rating += 40;
            if (cargo.age <= 30)
            {
                rating += 45;
                if (cargo.age <= 15)
                {
                    rating += 45;
                    if (cargo.age <= 7)
                    {
                        rating += 35;
                    }
                }
            }
        }

        // Penalty if lots of cargo waiting
        rating -= 130;
        if (cargo.quantity <= 1000)
        {
            rating += 30;
            if (cargo.quantity <= 500)
            {
                rating += 30;
                if (cargo.quantity <= 300)
                {
                    rating += 30;
                    if (cargo.quantity <= 200)
                    {
                        rating += 20;
                        if (cargo.quantity <= 100)
                        {
                            rating += 20;
                        }
                    }
                }
            }
        }

        if ((flags & (station_flags::flag_7 | station_flags::flag_8)) == 0 && !is_player_company(owner))
        {
            rating = 120;
        }

        int32_t unk3 = std::min<uint8_t>(cargo.var_36, 250);
        if (unk3 < 35)
        {
            rating += unk3 / 4;
        }

        if (cargo.var_38 < 4)
        {
            rating += 10;
            if (cargo.var_38 < 2)
            {
                rating += 10;
                if (cargo.var_38 < 1)
                {
                    rating += 13;
                }
            }
        }

        return std::clamp<int32_t>(rating, min_cargo_rating, max_cargo_rating);
    }

    void station::sub_4929DB()
    {
        registers regs;
        regs.ebp = (int32_t)this;
        call(0x004929DB, regs);
    }

    // 0x004CBA2D
    void station::invalidate()
    {
        ui::viewportmgr::invalidate(this);
    }

    void station::invalidate_window()
    {
        WindowManager::invalidate(WindowType::station, id());
    }

    // 0x0048F6D4
    static station_element* getStationElement(const map_pos3& pos)
    {
        auto tile = tilemgr::get(pos.x, pos.y);
        auto baseZ = pos.z / 4;

        for (auto& element : tile)
        {
            auto stationElement = element.as_station();

            if (stationElement == nullptr)
            {
                continue;
            }

            if (stationElement->base_z() != baseZ)
            {
                continue;
            }

            if (!stationElement->is_flag_5())
            {
                return stationElement;
            }
            else
            {
                return nullptr;
            }
        }
        return nullptr;
    }

    // 0x00491EDC
    static void setStationCatchmentRegion(CargoSearchState& cargoSearchState, TilePos minPos, TilePos maxPos, const uint8_t flag)
    {
        minPos.x = std::max(minPos.x, static_cast<coord_t>(0));
        minPos.y = std::max(minPos.y, static_cast<coord_t>(0));
        maxPos.x = std::min(maxPos.x, static_cast<coord_t>(map_columns - 1));
        maxPos.y = std::min(maxPos.y, static_cast<coord_t>(map_rows - 1));

        maxPos.x -= minPos.x;
        maxPos.y -= minPos.y;
        maxPos.x++;
        maxPos.y++;

        cargoSearchState.setTileRegion(minPos.x, minPos.y, maxPos.x, maxPos.y, flag);
    }

    // 0x00491BF5
    static void sub_491BF5(const map_pos& pos, const uint8_t flag)
    {
        TilePos minPos(pos);
        auto maxPos = minPos;
        maxPos.x += catchmentSize;
        maxPos.y += catchmentSize;
        minPos.x -= catchmentSize;
        minPos.y -= catchmentSize;

        CargoSearchState cargoSearchState;

        setStationCatchmentRegion(cargoSearchState, minPos, maxPos, flag);
    }
}
