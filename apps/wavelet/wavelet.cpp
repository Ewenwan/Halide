#include <stdio.h>

#include "haar_x.h"
#include "inverse_haar_x.h"
#include "daubechies_x.h"
#include "inverse_daubechies_x.h"

#include "static_image.h"
#include "image_io.h"

template<typename T>
T clamp(T x, T min, T max) {
    if (x < min) return min;
    if (x > max) return max;
    return x;
}

template<typename T>
void save_untransformed(Image<T> t, const std::string& filename) {
    save(t, filename);
    printf("Saved %s\n", filename.c_str());
}

template<typename T>
void save_transformed(Image<T> t, const std::string& filename) {
    Image<T> rearranged(t.width()*2, t.height(), 1);
    for (int y = 0; y < t.height(); y++) {
        for (int x = 0; x < t.width(); x++) {
            rearranged(x, y, 0) = clamp(t(x, y, 0), 0.0f, 1.0f);
            rearranged(x + t.width(), y, 0) = clamp(t(x, y, 1)*4.f + 0.5f, 0.0f, 1.0f);
        }
    }
    save(rearranged, filename);
    printf("Saved %s\n", filename.c_str());
}

int main(int argc, char **argv) {
    _assert(argc == 3, "Usage: main <src_image> <output-dir>\n");

    const std::string src_image = argv[1];
    const std::string dirname = argv[2];

    Image<float> input = load<float>(src_image);
    Image<float> transformed(input.width()/2, input.height(), 2);
    Image<float> inverse_transformed(input.width(), input.height(), 1);

    _assert(haar_x(input, transformed) == 0, "haar_x failed");
    save_transformed(transformed, dirname + "/haar_x.png");

    _assert(inverse_haar_x(transformed, inverse_transformed) == 0, "inverse_haar_x failed");
    save_untransformed(inverse_transformed, dirname + "/inverse_haar_x.png");

    _assert(daubechies_x(input, transformed) == 0, "daubechies_x failed");
    save_transformed(transformed, dirname + "/daubechies_x.png");

    _assert(inverse_daubechies_x(transformed, inverse_transformed) == 0, "inverse_daubechies_x failed");
    save_untransformed(inverse_transformed, dirname + "/inverse_daubechies_x.png");

    printf("Done.\n");
    return 0;
}
