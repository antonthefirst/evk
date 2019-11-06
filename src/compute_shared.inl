
#ifdef _WIN32
typedef unsigned int uint;
#endif

#ifdef _WIN32
struct ComputeUPC {
#else
layout(push_constant) uniform UPC {
#endif

	uint stage;

#ifdef _WIN32
};
#else
} upc;
#endif