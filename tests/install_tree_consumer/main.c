#include <keysharp_desktop/client.h>

int main(void)
{
    ksd_connect_options options;
    ksd_point point;
    ksd_rectangle rectangle;
    ksd_connect_options_init(&options);
    ksd_point_init(&point);
    ksd_rectangle_init(&rectangle);
    return options.struct_size == sizeof(options)
            && point.struct_size == sizeof(point)
            && rectangle.struct_size == sizeof(rectangle)
            && ksd_client_abi_major() == KSD_CLIENT_ABI_MAJOR
            && ksd_client_abi_minor() >= KSD_CLIENT_ABI_MINOR
        ? 0 : 1;
}
