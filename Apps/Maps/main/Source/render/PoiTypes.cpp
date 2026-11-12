// SPDX-License-Identifier: GPL-3.0-or-later
#include "PoiTypes.h"

#include <cstring>

namespace maps {

namespace {

constexpr uint8_t Y = 1;
constexpr uint8_t N = 0;

#define MAPS_POI_NAME(name, file) file,
const char* const ICON_NAMES[] = { MAPS_POI_ICONS(MAPS_POI_NAME) };
#undef MAPS_POI_NAME

// A subclass entry (S) wins over a class entry (C); the class entries cover every class that the tiles use.
#define S(key, icon, label, group, zoom, unnamed) { key, false, PoiIcon::icon, label, PoiGroup::group, zoom, unnamed == Y }
#define C(key, icon, label, group, zoom, unnamed) { key, true, PoiIcon::icon, label, PoiGroup::group, zoom, unnamed == Y }

// Entry n of the table is type id n + 1. Never reorder: ids are stored in prepared tiles (raise the format version).
constexpr PoiType TYPES[] = {
    // Subclasses
    S("bus_stop", Bus, "Bus stop", Transport, 15, Y),
    S("bus_station", Bus, "Bus station", Transport, 15, N),
    S("tram_stop", Rail, "Tram stop", Transport, 15, Y),
    S("station", Rail, "Railway station", Transport, 14, N),
    S("halt", Rail, "Railway halt", Transport, 15, N),
    S("subway", RailMetro, "Metro station", Transport, 14, N),
    S("subway_entrance", RailMetro, "Metro entrance", Transport, 16, N),
    S("bicycle_rental", BicycleShare, "Bicycle rental", Transport, 15, Y),
    S("charging_station", ChargingStation, "Charging station", Transport, 15, Y),
    S("car_repair", CarRepair, "Car repair", Transport, 15, N),
    S("car_parts", CarRepair, "Car parts", Shop, 15, N),
    S("taxi", Taxi, "Taxi", Transport, 15, N),
    S("ferry_terminal", Ferry, "Ferry", Transport, 15, N),
    S("supermarket", Grocery, "Supermarket", Shop, 15, N),
    S("convenience", Convenience, "Convenience store", Shop, 15, N),
    S("kiosk", Convenience, "Kiosk", Shop, 15, N),
    S("bakery", Bakery, "Bakery", Food, 15, N),
    S("butcher", Grocery, "Butcher", Food, 15, N),
    S("greengrocer", Grocery, "Greengrocer", Food, 15, N),
    S("deli", Grocery, "Deli", Food, 15, N),
    S("confectionery", Confectionery, "Sweets", Food, 15, N),
    S("chocolate", Confectionery, "Chocolate", Food, 15, N),
    S("marketplace", Grocery, "Market", Shop, 15, N),
    S("department_store", Shop, "Department store", Shop, 15, N),
    S("mall", Shop, "Shopping centre", Shop, 15, N),
    S("clothes", ClothingStore, "Clothes", Shop, 15, N),
    S("bag", ClothingStore, "Bags", Shop, 15, N),
    S("shoes", Shoe, "Shoes", Shop, 15, N),
    S("jewelry", Jewelry, "Jewellery", Shop, 15, N),
    S("beauty", Hairdresser, "Beauty", Shop, 15, N),
    S("hairdresser", Hairdresser, "Hairdresser", Shop, 15, N),
    S("mobile_phone", MobilePhone, "Phones", Shop, 15, N),
    S("florist", Florist, "Florist", Shop, 15, N),
    S("optician", Optician, "Optician", Health, 15, N),
    S("gift", Gift, "Gifts", Shop, 15, N),
    S("furniture", Furniture, "Furniture", Shop, 15, N),
    S("hardware", Hardware, "Hardware", Shop, 15, N),
    S("doityourself", Hardware, "DIY store", Shop, 15, N),
    S("books", Library, "Bookshop", Shop, 15, N),
    S("chemist", Pharmacy, "Drugstore", Shop, 15, N),
    S("pub", Beer, "Pub", Food, 15, N),
    S("biergarten", Beer, "Beer garden", Food, 15, N),
    S("nightclub", Nightclub, "Nightclub", Food, 15, N),
    S("hotel", Lodging, "Hotel", Lodging, 15, N),
    S("hostel", Lodging, "Hostel", Lodging, 15, N),
    S("guest_house", Lodging, "Guest house", Lodging, 15, N),
    S("motel", Lodging, "Motel", Lodging, 15, N),
    S("camp_site", Campsite, "Campsite", Lodging, 15, N),
    S("clinic", Hospital, "Clinic", Health, 15, N),
    S("nursing_home", Hospital, "Nursing home", Health, 15, N),
    S("kindergarten", School, "Kindergarten", Education, 15, N),
    S("university", College, "University", Education, 14, N),
    S("post_office", Post, "Post office", Public, 15, N),
    S("post_box", Post, "Post box", Public, 16, Y),
    S("parcel_locker", Post, "Parcel locker", Public, 16, Y),
    S("artwork", ArtGallery, "Artwork", Culture, 16, N),
    S("gallery", ArtGallery, "Gallery", Culture, 15, N),
    S("arts_centre", ArtGallery, "Arts centre", Culture, 15, N),
    S("viewpoint", Viewpoint, "Viewpoint", Culture, 15, N),
    S("community_centre", TownHall, "Community centre", Public, 15, N),
    S("courthouse", TownHall, "Court", Public, 15, N),
    S("government", TownHall, "Government office", Public, 15, N),
    S("diplomatic", Embassy, "Embassy", Public, 15, N),
    S("christian", Christian, "Church", Worship, 15, N),
    S("jewish", Jewish, "Synagogue", Worship, 15, N),
    S("muslim", Muslim, "Mosque", Worship, 15, N),
    S("buddhist", Buddhist, "Temple", Worship, 15, N),
    S("board", Information, "Information board", Other, 16, Y),
    S("guidepost", Information, "Guidepost", Other, 16, Y),
    S("map", Information, "Map", Other, 16, Y),
    S("route_marker", Information, "Route marker", Other, 16, Y),
    S("terminal", Information, "Information terminal", Other, 16, Y),
    S("swimming_pool", Swimming, "Swimming pool", Leisure, 15, N),
    S("sports_centre", Fitness, "Sports centre", Leisure, 15, N),
    S("water_park", Swimming, "Water park", Leisure, 15, N),
    S("theme_park", AmusementPark, "Theme park", Leisure, 14, N),
    // Classes
    C("shop", Shop, "Shop", Shop, 15, N),
    C("town_hall", TownHall, "Town hall", Public, 15, N),
    C("golf", Golf, "Golf", Leisure, 15, N),
    C("fast_food", FastFood, "Fast food", Food, 15, N),
    C("park", Park, "Park", Leisure, 15, N),
    C("bus", Bus, "Bus stop", Transport, 15, Y),
    C("railway", Rail, "Railway", Transport, 14, N),
    C("aerialway", Aerialway, "Aerialway", Transport, 15, N),
    C("entrance", Entrance, "Entrance", Transport, 16, N),
    C("campsite", Campsite, "Campsite", Lodging, 15, N),
    C("laundry", Laundry, "Laundry", Shop, 15, N),
    C("grocery", Grocery, "Grocery", Shop, 15, N),
    C("library", Library, "Library", Education, 15, N),
    C("college", College, "College", Education, 15, N),
    C("lodging", Lodging, "Lodging", Lodging, 15, N),
    C("ice_cream", IceCream, "Ice cream", Food, 15, N),
    C("post", Post, "Post", Public, 15, N),
    C("cafe", Cafe, "Cafe", Food, 15, N),
    C("school", School, "School", Education, 15, N),
    C("alcohol_shop", AlcoholShop, "Alcohol", Shop, 15, N),
    C("bar", Bar, "Bar", Food, 15, N),
    C("harbor", Harbor, "Harbour", Transport, 15, N),
    C("car", Car, "Car", Transport, 15, N),
    C("hospital", Hospital, "Hospital", Health, 14, N),
    C("cemetery", Cemetery, "Cemetery", Worship, 15, N),
    C("attraction", Attraction, "Attraction", Culture, 14, N),
    C("beer", Beer, "Pub", Food, 15, N),
    C("music", Music, "Music", Shop, 15, N),
    C("stadium", Stadium, "Stadium", Leisure, 15, N),
    C("art_gallery", ArtGallery, "Art gallery", Culture, 15, N),
    C("clothing_store", ClothingStore, "Clothes", Shop, 15, N),
    C("swimming", Swimming, "Swimming", Leisure, 15, N),
    C("castle", Castle, "Castle", Culture, 14, N),
    C("atm", Bank, "ATM", Money, 15, Y),
    C("fuel", Fuel, "Fuel", Transport, 15, N),
    C("restaurant", Restaurant, "Restaurant", Food, 15, N),
    C("pharmacy", Pharmacy, "Pharmacy", Health, 15, N),
    C("bank", Bank, "Bank", Money, 15, N),
    C("place_of_worship", PlaceOfWorship, "Place of worship", Worship, 15, N),
    C("museum", Museum, "Museum", Culture, 14, N),
    C("theatre", Theatre, "Theatre", Culture, 15, N),
    C("cinema", Cinema, "Cinema", Culture, 15, N),
    C("police", Police, "Police", Public, 15, N),
    C("fire_station", FireStation, "Fire station", Public, 15, N),
    C("toilets", Toilet, "Toilets", Public, 16, Y),
    C("drinking_water", DrinkingWater, "Drinking water", Public, 16, Y),
    C("playground", Playground, "Playground", Leisure, 15, Y),
    C("parking", Parking, "Parking", Transport, 15, Y),
    C("bicycle", Bicycle, "Bicycle shop", Shop, 15, N),
    C("bicycle_rental", BicycleShare, "Bicycle rental", Transport, 15, Y),
    C("information", Information, "Information", Other, 15, Y),
    C("zoo", Zoo, "Zoo", Leisure, 14, N),
    C("aquarium", Aquarium, "Aquarium", Leisure, 15, N),
    C("dentist", Dentist, "Dentist", Health, 15, N),
    C("doctors", Doctor, "Doctor", Health, 15, N),
    C("veterinary", Veterinary, "Veterinary", Health, 15, N),
    C("hairdresser", Hairdresser, "Hairdresser", Shop, 15, N),
    C("garden", Garden, "Garden", Leisure, 15, N),
    C("dog_park", DogPark, "Dog park", Leisure, 15, N),
    C("picnic_site", PicnicSite, "Picnic site", Leisure, 15, N),
    C("pitch", Pitch, "Sports pitch", Leisure, 15, N),
    C("sports_centre", Fitness, "Sports centre", Leisure, 15, N),
    C("swimming_pool", Swimming, "Swimming pool", Leisure, 15, N),
    C("butcher", Grocery, "Butcher", Food, 15, N),
    C("ferry_terminal", Ferry, "Ferry", Transport, 15, N),
    C("theme_park", AmusementPark, "Theme park", Leisure, 14, N),
    C("water_park", Swimming, "Water park", Leisure, 15, N),
    C("waste_basket", WasteBasket, "Waste basket", Other, 16, N),
    C("recycling", Recycling, "Recycling", Other, 16, N),
    C("monument", Monument, "Monument", Culture, 15, N),
};

#undef S
#undef C

constexpr int TYPE_COUNT = static_cast<int>(sizeof(TYPES) / sizeof(TYPES[0]));
static_assert(TYPE_COUNT <= 255, "type ids are stored in a byte");

constexpr PoiType UNKNOWN = { "", false, PoiIcon::Marker, "", PoiGroup::Other, 255, false };

bool matches(const char* key, const char* text, size_t length) {
    return strlen(key) == length && memcmp(key, text, length) == 0;
}

} // namespace

const char* poiIconName(PoiIcon icon) {
    const auto index = static_cast<size_t>(icon);
    return index < static_cast<size_t>(PoiIcon::Count) ? ICON_NAMES[index] : ICON_NAMES[0];
}

uint8_t poiTypeFor(const char* classText, size_t classLength, const char* subclassText, size_t subclassLength) {
    if (subclassText != nullptr && subclassLength > 0) {
        for (int i = 0; i < TYPE_COUNT; i++) {
            if (!TYPES[i].isClass && matches(TYPES[i].key, subclassText, subclassLength)) {
                return static_cast<uint8_t>(i + 1);
            }
        }
    }
    if (classText != nullptr && classLength > 0) {
        for (int i = 0; i < TYPE_COUNT; i++) {
            if (TYPES[i].isClass && matches(TYPES[i].key, classText, classLength)) {
                return static_cast<uint8_t>(i + 1);
            }
        }
    }
    return 0;
}

const PoiType& poiType(uint8_t id) {
    return id >= 1 && id <= TYPE_COUNT ? TYPES[id - 1] : UNKNOWN;
}

int poiTypeCount() {
    return TYPE_COUNT;
}

} // namespace maps
