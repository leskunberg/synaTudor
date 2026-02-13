#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <signal.h>
#include <tudor/log.h>
#include "cli.h"

static void print_record(RECGUID guid, enum tudor_finger finger, void *ctx) {
    printf("RECORD | identity index=%10u finger=%d\n", guid.PartA, finger);
}

static bool abort_cmd_loop = false;
static void sigint_handler(int sig) {
    if(abort_cmd_loop) {
        log_warn("Forcefully exiting!");
        exit(EXIT_FAILURE);
    }
    abort_cmd_loop = true;
}

void cli_main_loop(struct tudor_device *device) {
    signal(SIGINT, sigint_handler);

    //Command loop
    while(!abort_cmd_loop) {
        //Read command
        puts("");
        puts("Commands:");
        puts("  e - enroll finger");
        puts("  v - verify finger");
        puts("  i - identify finger");
        puts("  q - query information about enrolled fingers");
        puts("  w - wipe enrolled finger(s)");
        puts("  s - shutdown driver");

        printf("> ");
        char cmd = getchar();
        while(!abort_cmd_loop && isspace(cmd)) cmd = getchar();
        if(abort_cmd_loop) break;

        //Execute command
        switch(tolower(cmd)) {
            case 'e': {
                //Read identity GUID
                RECGUID guid = {0};
                printf("Enter identity index: ");
                scanf("%u", &guid.PartA);
                while(!abort_cmd_loop && getchar() != '\n') continue;
                if(abort_cmd_loop) goto cmdend;

                //Read finger
                enum tudor_finger finger;
                printf("Enter finger index (1-5 = right hand thumb - little finger | 6-10 = left hand thumb - little finger): ");
                scanf("%d", (int*) &finger);
                while(!abort_cmd_loop && getchar() != '\n') continue;
                if(abort_cmd_loop) goto cmdend;
                if(finger < TUDOR_FINGER_RH_THUMB || TUDOR_FINGER_LH_LITTLE_FINGER < finger) {
                    printf("Invalid finger index!");
                    goto cmdend;
                }

                //Do enrollment
                if(!tudor_enroll_start(device, guid, finger)) {
                    log_error("Error starting enrollment!");
                    goto cmdend;
                }

                puts("Put your finger on the sensor");
                while(true) {
                    //Update enrollment
                    bool is_done;
                    tudor_async_res_t async_res = NULL;
                    if(!tudor_enroll_capture(device, &is_done, &async_res) || !tudor_wait_async(async_res)) {
                        if(async_res) tudor_cleanup_async(async_res);

                        if(!is_done) {
                            log_warn("Retrying enrollment update...");
                            continue;
                        }

                        log_error("Error capturing sample for enrollment!");
                        tudor_enroll_discard(device);
                        goto cmdend;
                    }
                    tudor_cleanup_async(async_res);

                    if(is_done) break;
                    puts("Driver requested more samples");
                }

                //Commit the enrollment
                bool is_dupl;
                if(!tudor_enroll_commit(device, &is_dupl)) {
                    if(is_dupl) {
                        log_warn("Detected duplicate enrollment!");
                        tudor_enroll_discard(device);
                        goto cmdend;
                    }

                    log_error("Error committing enrollment!");
                    tudor_enroll_discard(device);
                    goto cmdend;
                }

                puts("Successfully enrolled finger");

                //Reopen device to refresh DLL's matcher session
                //The DLL loads identity metadata (0xA2 commands) only during init
                puts("Reopening device to reload matcher...");
                if(!tudor_reopen(device)) {
                    log_error("Error reopening device after enrollment!");
                    goto cmdend;
                }
                puts("Device reopened - matcher refreshed");
            } goto cmdend;
            case 'v': {
                //Read identity GUID
                RECGUID verify_guid = {0};
                printf("Enter identity index: ");
                scanf("%u", &verify_guid.PartA);
                while(!abort_cmd_loop && getchar() != '\n') continue;
                if(abort_cmd_loop) goto cmdend;

                //Check that the identity exists in the storage adapter
                int found = tudor_enumerate_records(device, &verify_guid, TUDOR_FINGER_ANY, NULL, NULL);
                if(found == 0) {
                    printf("No enrolled finger found for identity index %u\n", verify_guid.PartA);
                    goto cmdend;
                }

                //Use identify internally (VerifyFeatureSet always returns BAD_CAPTURE)
                bool verify_match = false;
                RECGUID id_guid = {0};
                enum tudor_finger id_finger = 0;

                int verify_retries = 0;
                while(true) {
                    puts("Put your finger on the sensor");
                    bool retry;
                    tudor_async_res_t async_res = NULL;
                    if(!tudor_identify(device, &retry, &verify_match, &id_guid, &id_finger, &async_res) || !tudor_wait_async(async_res)) {
                        if(async_res) tudor_cleanup_async(async_res);

                        if(retry && ++verify_retries < 5) {
                            log_warn("Retrying verify capture (attempt %d/5)...", verify_retries);
                            continue;
                        }

                        log_error("Error verifying captured sample!%s", retry ? " (max retries reached)" : "");
                        goto cmdend;
                    }
                    tudor_cleanup_async(async_res);
                    break;
                }

                //Check if identify matched the requested identity
                if(verify_match && id_guid.PartA == verify_guid.PartA) {
                    puts("VERIFICATION SUCCESS");
                } else if(verify_match) {
                    printf("VERIFICATION FAILURE (matched different identity: index=%u)\n", id_guid.PartA);
                } else {
                    puts("VERIFICATION FAILURE (no match found)");
                }
            } goto cmdend;
            case 'i': {
                bool found_match;
                RECGUID match_guid;
                enum tudor_finger match_finger;

                int identify_retries = 0;
                while(true) {
                    //Capture and identify sample
                    puts("Put your finger on the sensor");
                    bool retry;
                    tudor_async_res_t async_res = NULL;
                    if(!tudor_identify(device, &retry, &found_match, &match_guid, &match_finger, &async_res) || !tudor_wait_async(async_res)) {
                        if(async_res) tudor_cleanup_async(async_res);

                        if(retry && ++identify_retries < 5) {
                            log_warn("Retrying identify capture (attempt %d/5)...", identify_retries);
                            continue;
                        }

                        log_error("Error identifying captured sample!%s", retry ? " (max retries reached)" : "");
                        goto cmdend;
                    }
                    tudor_cleanup_async(async_res);
                    break;
                }   

                //Print match
                if(found_match) {
                    printf("Found match: identity index=%u finger=%d\n", match_guid.PartA, match_finger);
                } else puts("Found no match");
            } goto cmdend;
            case 'q': {
                //Read identity GUID
                bool all_guids = false;
                RECGUID guid = {0};
                printf("Enter identity index (invalid number for all): ");
                if(scanf("%u", &guid.PartA) < 1) all_guids = true;
                while(!abort_cmd_loop && getchar() != '\n') continue;
                if(abort_cmd_loop) goto cmdend;

                //Query records from storage adapter
                int num_matched = tudor_enumerate_records(device,
                    all_guids ? NULL : &guid, TUDOR_FINGER_ANY,
                    print_record, NULL);
                printf("Total: %d record(s)\n", num_matched);
            } goto cmdend;
            case 'w': {
                //Read identity GUID
                bool all_guids = false;
                RECGUID guid = {0};
                printf("Enter identity index (invalid number for all): ");
                if(scanf("%u", &guid.PartA) < 1) all_guids = true;
                while(!abort_cmd_loop && getchar() != '\n') continue;
                if(abort_cmd_loop) goto cmdend;

                //Read finger
                enum tudor_finger finger;
                printf("Enter finger index (invalid number for all): ");
                if(scanf("%d", (int*) &finger) < 1) finger = TUDOR_FINGER_ANY;
                while(!abort_cmd_loop && getchar() != '\n') continue;
                if(abort_cmd_loop) goto cmdend;

                //Ask for confirmation
                printf("Do you really want to wipe all enrolled fingers with identity index=%u finger=%d?\n", guid.PartA, finger);
                printf("y/n: ");
                char yn = getchar();
                while(!abort_cmd_loop && getchar() != '\n') continue;
                if(yn != 'y') {
                    puts("Aborted wipe");
                    goto cmdend;
                }

                //Wipe records
                int num_wiped = tudor_wipe_records(device, all_guids ? NULL: &guid, finger);
                printf("Successfully wiped %d enrolled finger(s)\n", num_wiped);
            } goto cmdend;
            case 's': abort_cmd_loop = true; goto cmdend;
            default: printf("Unknown command '%c'!\n", cmd);
        }
        cmdend:;
    }
}