/**
 * Profiled Benchmark - Demonstrates continuous performance monitoring
 * Compile with: gcc -O2 -DAETHER_PROFILE -march=native
 */

#include <stdio.h>
#include <stdlib.h>
#include "../../runtime/actors/actor_state_machine.h"
#include "../../runtime/utils/aether_runtime_profile.h"

int main() {
    printf("===============================================================\n");
    printf("     Aether Profiled Benchmark - Continuous Monitoring\n");
    printf("===============================================================\n");
    
    profile_init();
    
    // Simulate message processing
    Mailbox mbox;
    mailbox_init(&mbox);
    
    const int MESSAGES = 1000000;
    Message msg = {0};
    msg.type = 1;
    msg.sender_id = 0;
    msg.payload_int = 42;
    msg.payload_ptr = NULL;
    
    printf("\nProcessing %d messages...\n", MESSAGES);
    
    for (int i = 0; i < MESSAGES; i++) {
        mailbox_send(&mbox, msg);
        Message out;
        mailbox_receive(&mbox, &out);
    }
    
    // Print detailed profile
    profile_print_report(1);
    profile_print_summary(1);
    
    // Export to CSV for further analysis
    profile_dump_csv("profile_results.csv", 1);
    
    printf("\n===============================================================\n");
    printf("Profiling Tips:\n");
    printf("  - Compile with -DAETHER_PROFILE to enable\n");
    printf("  - Zero overhead when disabled (no #ifdef in production)\n");
    printf("  - Export to CSV for trend analysis\n");
    printf("  - Monitor regressions across commits\n");
    printf("===============================================================\n");
    
    return 0;
}
