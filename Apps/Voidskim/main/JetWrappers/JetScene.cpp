// Compiles Jet's Scene.cpp without a static constructor.
//
// Scene.cpp has a global `static Material s_bakedMat;`. Its constructor would run from .ctors /
// .init_array, which Tactility's ELF loader never runs, and on Xtensa an image with relocations into
// those sections is rejected outright ("Invalid ELF image"). The macro below turns that declaration
// into a redeclaration of the accessor defined here, so every use reaches storage that is
// constructed on first access instead.

#include "Material.hpp"

#include <new>

// In namespace Renderer, where Scene.cpp declares s_bakedMat
namespace Renderer {
static Material* hdBakedMaterial() {
    alignas(Material) static unsigned char storage[sizeof(Material)];
    static bool constructed = false;
    if (!constructed) {
        new (storage) Material();
        constructed = true;
    }
    return reinterpret_cast<Material*>(storage);
}
}

// "static Material s_bakedMat;" becomes "static Material (*hdBakedMaterial());"
#define s_bakedMat (*hdBakedMaterial())
#include "Scene.cpp"
#undef s_bakedMat
