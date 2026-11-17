// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace maps {

/**
 * The map icons for points of interest: from the Maki set (https://github.com/mapbox/maki, CC0), and guidepost,
 * map-board, hut, spring, cave, cliff and saddle drawn for this app (host/make_poi_icons.py).
 */
#define MAPS_POI_ICONS(X) \
    X(Marker, "marker") X(Cafe, "cafe") X(Restaurant, "restaurant") X(FastFood, "fast-food") X(Bar, "bar") \
    X(Beer, "beer") X(Bus, "bus") X(Rail, "rail") X(RailMetro, "rail-metro") X(Bicycle, "bicycle") \
    X(BicycleShare, "bicycle-share") X(Car, "car") X(CarRepair, "car-repair") X(Fuel, "fuel") \
    X(ChargingStation, "charging-station") X(Parking, "parking") X(Shop, "shop") X(Grocery, "grocery") \
    X(Bakery, "bakery") X(Convenience, "convenience") X(Pharmacy, "pharmacy") X(Hospital, "hospital") \
    X(Doctor, "doctor") X(Dentist, "dentist") X(Veterinary, "veterinary") X(Bank, "bank") X(Lodging, "lodging") \
    X(Campsite, "campsite") X(Museum, "museum") X(Theatre, "theatre") X(Cinema, "cinema") \
    X(ArtGallery, "art-gallery") X(Castle, "castle") X(Monument, "monument") X(Attraction, "attraction") \
    X(Zoo, "zoo") X(Park, "park") X(Playground, "playground") X(Swimming, "swimming") X(Stadium, "stadium") \
    X(Golf, "golf") X(Library, "library") X(College, "college") X(School, "school") X(TownHall, "town-hall") \
    X(Police, "police") X(FireStation, "fire-station") X(Post, "post") X(Toilet, "toilet") \
    X(DrinkingWater, "drinking-water") X(Information, "information") X(Christian, "religious-christian") \
    X(Jewish, "religious-jewish") X(Muslim, "religious-muslim") X(Buddhist, "religious-buddhist") \
    X(PlaceOfWorship, "place-of-worship") X(Cemetery, "cemetery") X(IceCream, "ice-cream") \
    X(AlcoholShop, "alcohol-shop") X(Music, "music") X(ClothingStore, "clothing-store") X(Laundry, "laundry") \
    X(Harbor, "harbor") X(Aerialway, "aerialway") X(Entrance, "entrance") X(WasteBasket, "waste-basket") \
    X(Pitch, "pitch") X(Hairdresser, "hairdresser") X(Jewelry, "jewelry-store") X(Optician, "optician") \
    X(Gift, "gift") X(Furniture, "furniture") X(MobilePhone, "mobile-phone") X(Hardware, "hardware") \
    X(Confectionery, "confectionery") X(Garden, "garden") X(DogPark, "dog-park") X(PicnicSite, "picnic-site") \
    X(Viewpoint, "viewpoint") X(Embassy, "embassy") X(Nightclub, "nightclub") X(Fitness, "fitness-centre") \
    X(Ferry, "ferry") X(Taxi, "taxi") X(Shoe, "shoe") X(Florist, "florist") X(AmusementPark, "amusement-park") \
    X(Aquarium, "aquarium") X(Recycling, "recycling")     X(Shelter, "shelter") X(Guidepost, "guidepost") X(MapBoard, "map-board") X(Hut, "hut") X(Spring, "spring")     X(Cave, "cave") X(Cliff, "cliff") X(Saddle, "saddle") X(Mountain, "mountain") X(Volcano, "volcano")     X(Barrier, "barrier") X(Gate, "gate") X(LiftGate, "lift-gate") X(Telephone, "telephone") X(Prison, "prison")     X(Commercial, "commercial") X(Tennis, "tennis") X(Basketball, "basketball") X(Bbq, "bbq") X(Toll, "toll")     X(Gaming, "gaming") X(Water, "water") X(Suitcase, "suitcase")

enum class PoiIcon : uint8_t {
#define MAPS_POI_ICON_ENUM(name, file) name,
    MAPS_POI_ICONS(MAPS_POI_ICON_ENUM)
#undef MAPS_POI_ICON_ENUM
    Count
};

/** The Maki icon file name without ".svg", "marker" for anything out of range. */
const char* poiIconName(PoiIcon icon);

/** What the sheet and the icon colour group a type under. */
enum class PoiGroup : uint8_t { Food, Shop, Transport, Health, Money, Lodging, Culture, Leisure, Education, Public, Worship, Other };

struct PoiType {
    /** An OpenMapTiles poi subclass, or class when isClass is set. */
    const char* key;
    /** Matches the class, used when no subclass entry matches. */
    bool isClass;
    PoiIcon icon;
    /** Plain English words for the sheet. */
    const char* label;
    PoiGroup group;
    /**
     * The map zoom from which the type is shown: 14 for important, 15 for most, 16 for small things, 17 for the rest.
     * The types that were added later (every class the tiles have, 0.8.6) start at 18, barriers at 19: from zoom 18 on
     * every POI is drawn, whatever its type and rank, so this only decides below it.
     */
    uint8_t minZoom;
    /** Worth showing on the map without a name (a toilet, a bus stop). */
    bool unnamedOk;
};

/**
 * The type id of a poi feature: the first subclass entry that matches the subclass text, else the class entry
 * that matches the class text. A class that nobody listed gets the generic type (poiGenericType()): a marker, drawn
 * with its class name when the feature has no name. Never 0. Texts need no terminator, nullptr is fine for none.
 */
uint8_t poiTypeFor(const char* classText, size_t classLength, const char* subclassText, size_t subclassLength);

/** The type of a POI whose class is not in the table. */
uint8_t poiGenericType();

/**
 * The type of a mountain_peak feature from its class (peak, volcano, saddle, cliff); anything else is a peak. Their
 * icons show next to the name and height.
 */
uint8_t poiTypeForPeak(const char* classText, size_t classLength);

/**
 * Whether a poi class is a barrier (gate, bollard, lift gate, cycle barrier, stile): drawn at zoom 19 only, and not
 * a place to go to, so the search leaves them out.
 */
bool poiClassIsBarrier(const char* classText, size_t classLength);

/** The type of an id from 1 to poiTypeCount(). Id 0 and anything above give an empty "unknown" type. */
const PoiType& poiType(uint8_t id);

/** How many types there are (at most 255): the valid ids are 1 to this. */
int poiTypeCount();

} // namespace maps
