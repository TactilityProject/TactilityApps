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
    S("board", MapBoard, "Information board", Other, 16, Y),
    S("guidepost", Guidepost, "Guidepost", Other, 16, Y),
    S("map", MapBoard, "Map", Other, 16, Y),
    S("route_marker", Guidepost, "Route marker", Other, 16, Y),
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
    // Added in 0.8.6, so that every class of the tiles has a type (host/poi_classes.py lists them). They start at zoom
    // 18, where every POI is drawn; unnamedOk only matters below that, so it stays N for them. The ids above never change.
    C("bakery", Bakery, "Bakery", Food, 18, N),
    C("shelter", Shelter, "Shelter", Other, 18, N),
    C("bicycle_parking", Bicycle, "Bicycle parking", Transport, 18, N),
    C("motorcycle_parking", Parking, "Motorcycle parking", Transport, 18, N),
    C("telephone", Telephone, "Telephone", Public, 18, N),
    C("office", Commercial, "Office", Other, 18, N),
    C("prison", Prison, "Prison", Public, 18, N),
    C("border_control", Barrier, "Border control", Public, 18, N),
    C("toll_booth", Toll, "Toll booth", Transport, 18, N),
    C("sally_port", Castle, "Sally port", Culture, 18, N),
    C("brownfield", Marker, "Brownfield", Other, 18, N),
    C("enclosure", Zoo, "Enclosure", Leisure, 18, N),
    C("basin", Water, "Basin", Other, 18, N),
    C("hackerspace", Marker, "Hackerspace", Public, 18, N),
    C("escape_game", Gaming, "Escape game", Leisure, 18, N),
    C("yoga", Fitness, "Yoga", Leisure, 18, N),
    C("ice_rink", Pitch, "Ice rink", Leisure, 18, N),
    C("climbing", Fitness, "Climbing", Leisure, 18, N),
    C("climbing_adventure", Fitness, "Climbing park", Leisure, 18, N),
    C("tennis", Tennis, "Tennis", Leisure, 18, N),
    C("table_tennis", Tennis, "Table tennis", Leisure, 18, N),
    C("basketball", Basketball, "Basketball", Leisure, 18, N),
    C("athletics", Pitch, "Athletics", Leisure, 18, N),
    C("chess", Pitch, "Chess", Leisure, 18, N),
    C("running", Pitch, "Running", Leisure, 18, N),
    C("cycling", Bicycle, "Cycling", Leisure, 18, N),
    C("gymnastics", Fitness, "Gymnastics", Leisure, 18, N),
    C("multi", Pitch, "Sports pitch", Leisure, 18, N),
    C("boxing", Fitness, "Boxing", Leisure, 18, N),
    C("judo", Fitness, "Judo", Leisure, 18, N),
    C("billiards", Pitch, "Billiards", Leisure, 18, N),
    C("shooting", Pitch, "Shooting", Leisure, 18, N),
    C("archery", Pitch, "Archery", Leisure, 18, N),
    C("baseball", Pitch, "Baseball", Leisure, 18, N),
    C("handball", Pitch, "Handball", Leisure, 18, N),
    C("volleyball", Pitch, "Volleyball", Leisure, 18, N),
    C("beachvolleyball", Pitch, "Beach volleyball", Leisure, 18, N),
    C("badminton", Pitch, "Badminton", Leisure, 18, N),
    C("bowls", Pitch, "Bowls", Leisure, 18, N),
    C("ice_hockey", Pitch, "Ice hockey", Leisure, 18, N),
    C("skateboard", Pitch, "Skate park", Leisure, 18, N),
    C("bmx", Bicycle, "BMX", Leisure, 18, N),
    C("equestrian", Pitch, "Equestrian", Leisure, 18, N),
    C("horse_racing", Pitch, "Horse racing", Leisure, 18, N),
    C("karting", Car, "Karting", Leisure, 18, N),
    C("motocross", Car, "Motocross", Leisure, 18, N),
    C("motor", Car, "Motor sports", Leisure, 18, N),
    C("rc_car", Car, "Model cars", Leisure, 18, N),
    C("rowing", Harbor, "Rowing", Leisure, 18, N),
    C("canoe", Harbor, "Canoe", Leisure, 18, N),
    C("sailing", Harbor, "Sailing", Leisure, 18, N),
    C("water_ski", Harbor, "Water skiing", Leisure, 18, N),
    C("toboggan", Pitch, "Toboggan run", Leisure, 18, N),
    C("orienteering", Pitch, "Orienteering", Leisure, 18, N),
    C("disc_golf", Golf, "Disc golf", Leisure, 18, N),
    // Barriers are not places to go to: zoom 19, and the search leaves them out
    C("gate", Gate, "Gate", Other, 19, N),
    C("lift_gate", LiftGate, "Lift gate", Other, 19, N),
    C("bollard", Barrier, "Bollard", Other, 19, N),
    C("cycle_barrier", Barrier, "Cycle barrier", Other, 19, N),
    C("stile", Barrier, "Stile", Other, 19, N),
    // The mountain_peak layer: its class is the type (poiTypeForPeak). They show by rank from zoom 10, see LabelLayout.
    C("peak", Mountain, "Peak", Other, 10, N),
    C("volcano", Volcano, "Volcano", Other, 10, N),
    C("saddle", Saddle, "Saddle", Other, 10, N),
    C("cliff", Cliff, "Cliff", Other, 10, N),
    // Subclasses of classes that had a type before deserve their own words and icon; they keep the zoom of their class
    S("alpine_hut", Hut, "Alpine hut", Lodging, 15, N),
    S("wilderness_hut", Hut, "Wilderness hut", Lodging, 15, N),
    S("chalet", Lodging, "Chalet", Lodging, 15, N),
    S("ruins", Castle, "Ruins", Culture, 14, N),
    S("bbq", Bbq, "Barbecue", Leisure, 15, N),
    S("spring", Spring, "Spring", Other, 18, N),
    S("cave_entrance", Cave, "Cave", Other, 18, N),
    S("visitor_centre", Information, "Visitor centre", Public, 15, Y),
    S("office", Information, "Information office", Public, 15, Y),
    S("tactile_map", MapBoard, "Tactile map", Other, 15, Y),
    S("trail_blaze", Guidepost, "Trail blaze", Other, 15, Y),
    S("pet", Shop, "Pet shop", Shop, 15, N),
    S("toys", Shop, "Toy shop", Shop, 15, N),
    S("sports", Shop, "Sports shop", Shop, 15, N),
    S("outdoor", Shop, "Outdoor shop", Shop, 15, N),
    S("electronics", Shop, "Electronics", Shop, 15, N),
    S("computer", Shop, "Computers", Shop, 15, N),
    S("stationery", Shop, "Stationery", Shop, 15, N),
    S("newsagent", Shop, "Newsagent", Shop, 15, N),
    S("tobacco", Shop, "Tobacco", Shop, 15, N),
    S("copyshop", Shop, "Copy shop", Shop, 15, N),
    S("travel_agency", Shop, "Travel agency", Shop, 15, N),
    S("tailor", Shop, "Tailor", Shop, 15, N),
    S("second_hand", Shop, "Second hand", Shop, 15, N),
    S("garden_centre", Shop, "Garden centre", Shop, 15, N),
    S("perfumery", Shop, "Perfumery", Shop, 15, N),
    S("cosmetics", Shop, "Cosmetics", Shop, 15, N),
    S("massage", Shop, "Massage", Shop, 15, N),
    S("tattoo", Shop, "Tattoo", Shop, 15, N),
    S("watches", Shop, "Watches", Shop, 15, N),
    S("motorcycle", Shop, "Motorcycles", Shop, 15, N),
    S("antiques", Shop, "Antiques", Shop, 15, N),
    S("ticket", Shop, "Tickets", Shop, 15, N),
    S("locksmith", Shop, "Locksmith", Shop, 15, N),
    S("paint", Shop, "Paint shop", Shop, 15, N),
    // Last: the type of a class nobody listed, so ids stay valid when more are added above
    { "", false, PoiIcon::Marker, "Place", PoiGroup::Other, 18, false },
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

uint8_t poiGenericType() {
    return static_cast<uint8_t>(TYPE_COUNT);
}

uint8_t poiTypeForPeak(const char* classText, size_t classLength) {
    // The mountain_peak classes are class entries of the table; anything else is drawn as a peak
    static const char* const OTHERS[] = { "volcano", "saddle", "cliff" };
    const char* wanted = "peak";
    for (const char* key : OTHERS) {
        if (classText != nullptr && matches(key, classText, classLength)) {
            wanted = key;
        }
    }
    for (int i = 0; i < TYPE_COUNT; i++) {
        if (TYPES[i].isClass && strcmp(TYPES[i].key, wanted) == 0) {
            return static_cast<uint8_t>(i + 1);
        }
    }
    return poiGenericType();
}

bool poiClassIsBarrier(const char* classText, size_t classLength) {
    static const char* const BARRIERS[] = { "gate", "lift_gate", "bollard", "cycle_barrier", "stile" };
    for (const char* key : BARRIERS) {
        if (classText != nullptr && matches(key, classText, classLength)) {
            return true;
        }
    }
    return false;
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
    return poiGenericType();
}

const PoiType& poiType(uint8_t id) {
    return id >= 1 && id <= TYPE_COUNT ? TYPES[id - 1] : UNKNOWN;
}

int poiTypeCount() {
    return TYPE_COUNT;
}

} // namespace maps
