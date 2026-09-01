#include "mcl/ap_channel.h"

/* No standard header included before mcl/ap_channel.h */

int main(void)
{
    /* Use an API requiring size_t and double */
    double t = mcl_ap_serialization_time_s((size_t)16, 1000.0);
    return (t > 0.0) ? 0 : 1;
}
