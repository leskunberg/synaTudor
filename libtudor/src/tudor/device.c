#include <unistd.h>
#include "internal.h"

static void req_cb(struct winwdf_request *req, NTSTATUS status, OVERLAPPED *ovlp) {
    //Get request info
    const void *out_buf = NULL;
    size_t out_buf_size, num_transfered = 0;
    if(status == STATUS_SUCCESS) {
        if(!winwdf_get_request_info(req, NULL, NULL, NULL, &out_buf, &out_buf_size, &num_transfered)) {
            log_error("Couldn't get request info!");
            abort();
        }
        if(num_transfered > out_buf_size) num_transfered = out_buf_size;
    }

    if(LOG_LEVEL <= LOG_VERBOSE) {
        cant_fail_ret(pthread_mutex_lock(&LOG_LOCK));
        printf("[DEVCTRL] <- status 0x%x out (size 0x%lx): ", status, num_transfered);
        if(status == STATUS_SUCCESS) for(size_t i = 0; i < num_transfered; i++) printf("%02x", ((uint8_t*) out_buf)[i]);
        puts("");
        cant_fail_ret(pthread_mutex_unlock(&LOG_LOCK));
    }

    //Complete the OVERLAPPED
    winio_complete_overlapped(ovlp, status, num_transfered);
}

static NTSTATUS tudor_devctrl(struct tudor_device *device, OVERLAPPED *ovlp, ULONG code, void *in_buf, size_t in_size, void *out_buf, size_t out_size, struct winwdf_request **req) {
    if(LOG_LEVEL <= LOG_VERBOSE) {
        cant_fail_ret(pthread_mutex_lock(&LOG_LOCK));
        printf("[DEVCTRL] -> in code 0x%x (size 0x%lx): ", code, in_size);
        for(size_t i = 0; i < in_size; i++) printf("%02x", ((uint8_t*) in_buf)[i]);
        puts("");
        cant_fail_ret(pthread_mutex_unlock(&LOG_LOCK));
    }

    //Start the request
    struct winmodule *mod = winmodule_get_cur();
    winmodule_set_cur(&tudor_driver_dll->module);
    NTSTATUS status = winwdf_devctrl_file(device->wdf_file, code, in_buf, in_size, out_buf, out_size, req);
    winmodule_set_cur(mod);

    //Override template count to prevent DATABASE_FULL during enrollment only.
    //During init/reopen, the DLL needs the real count to trigger DB_INIT (0xA5 01),
    //which loads biometric templates into the matching engine.
    if(code == 0x44202c && status == STATUS_SUCCESS && out_size >= 4) {
        uint32_t val = *(uint32_t *)out_buf;
        if(val > 0 && device->override_template_count) {
            log_info("tudor_devctrl: IOCTL 0x44202c reported %u templates, overriding to 0 (enrollment mode)", val);
            *(uint32_t *)out_buf = 0;
        } else if(val > 0) {
            log_info("tudor_devctrl: IOCTL 0x44202c reported %u templates (not overriding)", val);
        }
    }

    //Add callback
    if(status == STATUS_SUCCESS) winwdf_add_request_callback(*req, (winwdf_request_cb_fnc*) req_cb, ovlp);

    return status;
}

static NTSTATUS tudor_cancel(struct tudor_device *device, OVERLAPPED *ovlp, struct winwdf_request *req) {
    winwdf_cancel_request(req);
    return STATUS_SUCCESS;
}

static void tudor_cleanup(struct tudor_device *device, OVERLAPPED *ovlp, struct winwdf_request *req) {
    winwdf_destroy_object((WDFOBJECT) req);
}

bool tudor_open(struct tudor_device *device, int hidraw_fd, struct tudor_device_state *state) {
    HRESULT hres;
    NTSTATUS status;

    log_info("tudor_open: ENTER (hidraw_fd=%d, state=%p)", hidraw_fd, state);

    device->hidraw_fd = hidraw_fd;
    device->state = state ? *state : (struct tudor_device_state) {0};
    device->enrolling = false;
    device->override_template_count = false;
    cant_fail_ret(pthread_mutex_init(&device->records_lock, NULL));
    device->records_head = NULL;
    device->result_records_head = device->result_records_cursor = NULL;

    //Open the device through the driver
    log_info("tudor_open: opening registry key...");
    device->reg_key = winreg_open_key(device, "HKEY_LOCAL_MACHINE\\Tudor\\Device");
    log_info("tudor_open: calling winwdf_add_device...");
    if((status = winwdf_add_device(tudor_wdf_driver, device->reg_key, hidraw_fd, &device->wdf_device)) != 0) {
        log_error("Error adding WDF device: 0x%x!", status);
        return false;
    }
    log_info("tudor_open: winwdf_add_device returned OK (wdf_device=%p)", device->wdf_device);
    if(!device->wdf_device) {
        log_error("Driver didn't create a WDF device!");
        return false;
    }
    log_info("tudor_open: calling winwdf_event_queue_flush (this runs PnP callbacks)...");
    winwdf_event_queue_flush();
    log_info("tudor_open: winwdf_event_queue_flush returned");

    log_info("tudor_open: calling winwdf_open_device...");
    if((status = winwdf_open_device(device->wdf_device, &device->wdf_file)) != 0) {
        log_error("Error opening WDF file: 0x%x!", status);
        return false;
    }
    log_info("tudor_open: winwdf_open_device returned OK");

    //This is dumb, but otherwise we run into race conditions
    log_info("tudor_open: sleeping 3 seconds for race condition workaround...");
    cant_fail(usleep(3000000));
    log_info("tudor_open: sleep done");

    //Initialize the pipeline
    winmodule_set_cur(&tudor_adapter_dll->module);

    //Use the DLL's storage adapter if available — it loads templates from the sensor
    //and triggers the engine's matcher initialization (0xA2/0xA5 commands).
    //Fall back to our stub storage if the DLL's adapter isn't available.
    WINBIO_STORAGE_INTERFACE *storage = tudor_dll_storage_adapter ? tudor_dll_storage_adapter : tudor_storage_adapter;
    device->use_dll_storage = (storage == tudor_dll_storage_adapter);
    if(device->use_dll_storage) {
        log_info("Using DLL storage adapter for pipeline (sensor-side template management)");
    } else {
        log_info("Using stub storage adapter for pipeline");
    }

    log_debug("Initializing WINBIO pipeline...");
    device->pipeline = (WINBIO_PIPELINE*) malloc(sizeof(WINBIO_PIPELINE));
    if(!device->pipeline) { perror("Error allocating WINBIO pipeline"); abort(); }
    *device->pipeline = (WINBIO_PIPELINE) {0};
    device->pipeline->EngineInterface = tudor_engine_adapter;
    device->pipeline->SensorInterface = tudor_sensor_adapter;
    device->pipeline->StorageInterface = storage;
    device->pipeline->SensorHandle = device->winbio_file = winio_create_file(device, true, NULL, NULL, (winio_devctrl_fnc*) tudor_devctrl, (winio_cancel_fnc*) tudor_cancel, (winio_cleanup_fnc*) tudor_cleanup, NULL);
    device->pipeline->EngineHandle = INVALID_HANDLE_VALUE;
    device->pipeline->StorageHandle = INVALID_HANDLE_VALUE;
    device->pipeline->StorageContext = device->use_dll_storage ? NULL : device;

    log_debug("Attaching interfaces to pipeline...");
    WINBIO_CALL_PIPELINE(tudor_sensor_adapter->Attach, device->pipeline);
    WINBIO_CALL_PIPELINE(tudor_engine_adapter->Attach, device->pipeline);
    WINBIO_CALL_PIPELINE(storage->Attach, device->pipeline);

    log_debug("Initializing pipeline interfaces...");
    WINBIO_CALL_PIPELINE(tudor_sensor_adapter->PipelineInit, device->pipeline);
    WINBIO_CALL_PIPELINE(tudor_engine_adapter->PipelineInit, device->pipeline);
    WINBIO_CALL_PIPELINE(storage->PipelineInit, device->pipeline);

    //Open the database — this loads templates from the sensor into the storage adapter
    if(device->use_dll_storage && storage->OpenDatabase) {
        GUID db_id = DEFINE_GUID(d6612bd6, ecc0, 40bf, 80ff, 7e480722ad8c);
        static const char16_t empty[] = u"";
        log_debug("Opening storage database...");
        WINBIO_CALL_PIPELINE(storage->OpenDatabase, device->pipeline, &db_id, empty, empty);
    }

    //Reset the sensor
    log_debug("Resetting sensor...");
    WINBIO_CALL_PIPELINE(tudor_sensor_adapter->Reset, device->pipeline);

    //Activate the pipeline
    log_debug("Activating pipeline...");
    WINBIO_CALL_PIPELINE(tudor_sensor_adapter->Activate, device->pipeline);
    WINBIO_CALL_PIPELINE(tudor_engine_adapter->Activate, device->pipeline);
    WINBIO_CALL_PIPELINE(storage->Activate, device->pipeline);

    //Check the sensor status
    log_debug("Checking sensor status...");
    ULONG sensor_status = WINBIO_SENSOR_FAILURE;
    WINBIO_CALL_PIPELINE(tudor_sensor_adapter->QueryStatus, device->pipeline, &sensor_status)
    if(sensor_status != WINBIO_SENSOR_READY) {
        log_error("Sensor didn't return ready status! [sensor_status 0x%x]", sensor_status);
        return false;
    }

    return true;
}

bool tudor_close(struct tudor_device *device) {
    HRESULT hres;

    winmodule_set_cur(&tudor_adapter_dll->module);

    WINBIO_STORAGE_INTERFACE *storage = device->use_dll_storage ? tudor_dll_storage_adapter : tudor_storage_adapter;

    //Deactivate the pipeline
    log_debug("Deactivating pipeline...");
    WINBIO_CALL_PIPELINE(tudor_sensor_adapter->Deactivate, device->pipeline);
    WINBIO_CALL_PIPELINE(tudor_engine_adapter->Deactivate, device->pipeline);
    WINBIO_CALL_PIPELINE(storage->Deactivate, device->pipeline);

    //Close the database if using DLL storage
    if(device->use_dll_storage && storage->CloseDatabase) {
        log_debug("Closing storage database...");
        WINBIO_CALL_PIPELINE(storage->CloseDatabase, device->pipeline);
    }

    //Uninitialize the pipeline
    log_debug("Uninitializing pipeline interfaces...");
    WINBIO_CALL_PIPELINE(tudor_sensor_adapter->PipelineCleanup, device->pipeline);
    WINBIO_CALL_PIPELINE(tudor_engine_adapter->PipelineCleanup, device->pipeline);
    WINBIO_CALL_PIPELINE(storage->PipelineCleanup, device->pipeline);

    log_debug("Detaching interfaces from pipeline...");
    WINBIO_CALL_PIPELINE(tudor_sensor_adapter->Detach, device->pipeline);
    WINBIO_CALL_PIPELINE(tudor_engine_adapter->Detach, device->pipeline);
    WINBIO_CALL_PIPELINE(storage->Detach, device->pipeline);
    free(device->pipeline);

    winhandle_destroy(device->winbio_file);

    //Close the WDF file
    winmodule_set_cur(&tudor_driver_dll->module);
    winwdf_close_file(device->wdf_file);

    //Remove the device
    winmodule_set_cur(&tudor_driver_dll->module);
    winwdf_remove_device(device->wdf_device);
    winwdf_event_queue_flush();
    winhandle_destroy(device->reg_key);

    //Free records
    cant_fail_ret(pthread_mutex_lock(&device->records_lock));
    for(struct tudor_record *rec = device->records_head, *nrec = rec ? rec->next : NULL; rec; rec = nrec, nrec = rec ? rec->next : NULL) {
        free(rec->data);
        free(rec->identity);
        free(rec);
    }
    cant_fail_ret(pthread_mutex_unlock(&device->records_lock));
    cant_fail_ret(pthread_mutex_destroy(&device->records_lock));

    return true;
}

bool tudor_reopen(struct tudor_device *device) {
    int fd = device->hidraw_fd;
    log_info("tudor_reopen: closing and reopening device (fd=%d)...", fd);

    if(!tudor_close(device)) {
        log_error("tudor_reopen: close failed!");
        return false;
    }

    if(!tudor_open(device, fd, NULL)) {
        log_error("tudor_reopen: open failed!");
        return false;
    }

    log_info("tudor_reopen: device reopened successfully");
    return true;
}

int tudor_enumerate_records(struct tudor_device *device, RECGUID *guid, enum tudor_finger finger, tudor_record_cb_fnc *cb, void *ctx) {
    winmodule_set_cur(&tudor_adapter_dll->module);

    WINBIO_STORAGE_INTERFACE *storage = device->pipeline->StorageInterface;
    WINBIO_IDENTITY ident;
    if(guid) {
        ident.Type = WINBIO_ID_TYPE_GUID;
        ident.TemplateGuid = *(GUID*)(void*)guid;
    } else {
        ident.Type = WINBIO_ID_TYPE_WILDCARD;
        ident.Wildcard = 0x25066282; // WINBIO_IDENTITY_WILDCARD
    }

    log_debug("tudor_enumerate_records: QueryBySubject type=%d finger=%d", ident.Type, finger);
    HRESULT hr = storage->QueryBySubject(device->pipeline, &ident, (UCHAR)finger);
    if(hr != ERROR_SUCCESS) {
        log_warn("tudor_enumerate_records: QueryBySubject failed: 0x%08x", hr);
        return 0;
    }

    int count = 0;
    hr = storage->FirstRecord(device->pipeline);
    if(hr != ERROR_SUCCESS) {
        log_warn("tudor_enumerate_records: FirstRecord failed: 0x%08x", hr);
        return 0;
    }
    while(hr == ERROR_SUCCESS) {
        WINBIO_STORAGE_RECORD srec = {0};
        hr = storage->GetCurrentRecord(device->pipeline, &srec);
        if(hr != ERROR_SUCCESS) {
            log_warn("tudor_enumerate_records: GetCurrentRecord failed: 0x%08x", hr);
            break;
        }

        if(!srec.Identity) {
            log_warn("tudor_enumerate_records: GetCurrentRecord returned NULL Identity");
            break;
        }
        if(cb) cb(*(RECGUID*)(void*)&srec.Identity->TemplateGuid, (enum tudor_finger)srec.SubFactor, ctx);
        count++;

        hr = storage->NextRecord(device->pipeline);
    }
    return count;
}

bool tudor_enroll_start(struct tudor_device *device, RECGUID guid, enum tudor_finger finger) {
    winmodule_set_cur(&tudor_adapter_dll->module);
    HRESULT hres;

    if(device->enrolling) {
        log_error("Already enrolling a finger!");
        return false;
    }

    //Don't erase sensor templates - old templates with persisted identity data
    //are needed for the DLL's background thread to call DB_INIT (0xA5 01),
    //which loads biometric templates into the matching engine.
    //The template count override in tudor_devctrl handles DATABASE_FULL.

    //Follow https://docs.microsoft.com/en-us/windows/win32/secbiomet/adapter-workflow - WinBioEnrollBegin
    WINBIO_CALL_PIPELINE(tudor_sensor_adapter->ClearContext, device->pipeline);
    WINBIO_CALL_PIPELINE(tudor_engine_adapter->ClearContext, device->pipeline);
    WINBIO_CALL_PIPELINE(tudor_storage_adapter->ClearContext, device->pipeline);
    WINBIO_CALL_PIPELINE(tudor_engine_adapter->CreateEnrollment, device->pipeline);
    WINBIO_CALL_PIPELINE(tudor_engine_adapter->SetEnrollmentParameters, device->pipeline, &(WINBIO_EXTENDED_ENROLLMENT_PARAMETERS) {
        .Size = sizeof(WINBIO_EXTENDED_ENROLLMENT_PARAMETERS),
        .SubFactor = (UCHAR) finger
    });

    device->enrolling = true;
    device->override_template_count = true;
    device->enroll_guid = guid;
    device->enroll_finger = finger;
    return true;
}

static void enroll_cb(OVERLAPPED *ovlp, NTSTATUS status, tudor_async_res_t res) {
    winmodule_set_cur(&tudor_adapter_dll->module);
    HRESULT hres;
    bool success = false, done = true;

    if(status != STATUS_SUCCESS) {
        log_error("Error starting capture: 0x%x!", status);
        goto exit;
    }

    //Follow https://docs.microsoft.com/en-us/windows/win32/secbiomet/adapter-workflow - WinBioEnrollCapture
    ULONG reject_detail;
    if((hres = tudor_sensor_adapter->FinishCapture(res->dev->pipeline, &reject_detail)) != ERROR_SUCCESS) {
        if(hres == WINBIO_E_BAD_CAPTURE) done = false;
        log_error("Error finishing sensor capture: 0x%x! [reject detail 0x%x]", hres, reject_detail);
        goto exit;
    };
    if((hres = tudor_sensor_adapter->PushDataToEngine(res->dev->pipeline, WINBIO_PURPOSE_ENROLL, 0, &reject_detail)) != ERROR_SUCCESS) {
        if(hres == WINBIO_E_BAD_CAPTURE) done = false;
        log_error("Error pushing sensor data to engine: 0x%x! [reject detail 0x%x]", hres, reject_detail);
        goto exit;
    };

    log_debug("Updating enrollment...");
    if((hres = tudor_engine_adapter->UpdateEnrollment(res->dev->pipeline, &reject_detail)) != ERROR_SUCCESS && hres != WINBIO_I_MORE_DATA) {
        if(hres == WINBIO_E_BAD_CAPTURE) done = false;
        log_error("Error updating enrollment: 0x%x! [reject detail 0x%x]", hres, reject_detail);
        goto exit;
    };

    success = true;
    done = hres != WINBIO_I_MORE_DATA;

    exit:;
    *res->args.enroll.done = done;
    async_complete_op(res, success);
}

bool tudor_enroll_capture(struct tudor_device *device, bool *done, tudor_async_res_t *res) {
    winmodule_set_cur(&tudor_adapter_dll->module);
    *res = NULL;
    HRESULT hres;

    *done = true;
    if(!device->enrolling) {
        log_error("Not currently enrolling a finger!");
        return false;
    }

    log_debug("Capturing sample...");
    OVERLAPPED *ovlp;
    WINBIO_CALL_PIPELINE(tudor_sensor_adapter->StartCapture, device->pipeline, WINBIO_PURPOSE_ENROLL, &ovlp);

    *res = async_new_res(device, ovlp);
    (*res)->args.enroll = (struct async_args_enroll) { .done = done };
    winio_set_overlapped_callback(ovlp, (winio_overlapped_cb_fnc*) enroll_cb, *res, true);
    return true;
}

bool tudor_enroll_commit(struct tudor_device *device, bool *is_duplicate) {
    winmodule_set_cur(&tudor_adapter_dll->module);
    HRESULT hres;

    *is_duplicate = false;
    if(!device->enrolling) {
        log_error("Not currently enrolling a finger!");
        return false;
    }

    //Follow https://docs.microsoft.com/en-us/windows/win32/secbiomet/adapter-workflow - WinBioEnrollCommit
    log_debug("Checking for duplicate enrollment...");
    BOOLEAN is_dupl;
    WINBIO_IDENTITY dupl_ident;
    UCHAR dupl_finger;
    WINBIO_CALL_PIPELINE(tudor_engine_adapter->CheckForDuplicate, device->pipeline, &dupl_ident, &dupl_finger, &is_dupl);
    if(is_dupl) {
        *is_duplicate = true;
        return false;
    }

    log_debug("Committing enrollment...");

    //The driver doesn't support enrollment hashes (so we don't call GetEnrollmentHash)
    if((hres = tudor_engine_adapter->CommitEnrollment(device->pipeline, &(WINBIO_IDENTITY) {
        .Type = WINBIO_ID_TYPE_GUID,
        .TemplateGuid = *(GUID*) &device->enroll_guid
    }, (UCHAR) device->enroll_finger, NULL, 0)) != ERROR_SUCCESS) {
        log_error("Error committing enrollment: 0x%x!", hres);
        if(hres == WINBIO_E_DUPLICATE_ENROLLMENT) *is_duplicate = true;
        return false;
    }

    device->enrolling = false;
    device->override_template_count = false;
    return true;
}

bool tudor_enroll_discard(struct tudor_device *device) {
    winmodule_set_cur(&tudor_adapter_dll->module);
    HRESULT hres;

    if(!device->enrolling) {
        log_error("Not currently enrolling a finger!");
        return false;
    }

    log_info("Discarding enrollment of guid=%08x... finger=%x", device->enroll_guid.PartA, device->enroll_finger);

    //Follow https://docs.microsoft.com/en-us/windows/win32/secbiomet/adapter-workflow - WinBioEnrollDiscard
    WINBIO_CALL_PIPELINE(tudor_engine_adapter->DiscardEnrollment, device->pipeline);
    WINBIO_CALL_PIPELINE(tudor_engine_adapter->ClearContext, device->pipeline);
    WINBIO_CALL_PIPELINE(tudor_sensor_adapter->ClearContext, device->pipeline);

    device->enrolling = false;
    device->override_template_count = false;
    return true;
}

static void verify_cb(OVERLAPPED *ovlp, NTSTATUS status, tudor_async_res_t res) {
    winmodule_set_cur(&tudor_adapter_dll->module);
    HRESULT hres;
    bool success = false, retry = false, matches = false;

    if(status != STATUS_SUCCESS) {
        log_error("Error starting capture: 0x%x!", status);
        goto exit;
    }

    ULONG reject_detail;
    if((hres = tudor_sensor_adapter->FinishCapture(res->dev->pipeline, &reject_detail)) != ERROR_SUCCESS) {
        if(hres == WINBIO_E_BAD_CAPTURE) retry = true;
        log_error("Error finishing sensor capture: 0x%x! [reject detail 0x%x]", hres, reject_detail);
        goto exit;
    };
    if((hres = tudor_sensor_adapter->PushDataToEngine(res->dev->pipeline, WINBIO_PURPOSE_VERIFY, 0, &reject_detail)) != ERROR_SUCCESS) {
        if(hres == WINBIO_E_BAD_CAPTURE) retry = true;
        log_error("Error pushing sensor data to engine: 0x%x! [reject detail 0x%x]", hres, reject_detail);
        goto exit;
    };

    log_debug("Verifying sample...");
    BOOLEAN is_match;
    UCHAR *payload_ptr, *hash_ptr;
    SIZE_T payload_size, hash_size;
    if((hres = tudor_engine_adapter->VerifyFeatureSet(res->dev->pipeline, &(WINBIO_IDENTITY) {
        .Type = WINBIO_ID_TYPE_GUID,
        .TemplateGuid = *(GUID*) &res->args.verify.guid
    }, (UCHAR) res->args.verify.finger, &is_match, &payload_ptr, &payload_size, &hash_ptr, &hash_size, &reject_detail)) != ERROR_SUCCESS) {
        if(hres == WINBIO_E_BAD_CAPTURE) retry = true;
        if(hres == WINBIO_E_NO_MATCH) {
            success = true;
            matches = false;
            goto exit;
        }

        log_error("Error verifying sample: 0x%x! [reject detail 0x%x]", hres, reject_detail);
        goto exit;
    };

    success = true;
    matches = is_match;

    exit:;
    *(res->args.verify.retry) = retry;
    *(res->args.verify.matches) = matches;
    async_complete_op(res, success);
}

bool tudor_verify(struct tudor_device *device, RECGUID guid, enum tudor_finger finger, bool *retry, bool *matches, tudor_async_res_t *res) {
    winmodule_set_cur(&tudor_adapter_dll->module);
    *res = NULL;
    HRESULT hres;

    *retry = false;
    *matches = false;
    if(device->enrolling) {
        log_error("Currently enrolling a finger!");
        return false;
    }

    log_debug("Capturing sample...");
    OVERLAPPED *ovlp;
    WINBIO_CALL_PIPELINE(tudor_sensor_adapter->StartCapture, device->pipeline, WINBIO_PURPOSE_VERIFY, &ovlp);

    *res = async_new_res(device, ovlp);
    (*res)->args.verify = (struct async_args_verify) { .retry = retry, .guid = guid, .finger = finger, .matches = matches };
    winio_set_overlapped_callback(ovlp, (winio_overlapped_cb_fnc*) verify_cb, *res, true);
    return true;
}

static void identify_cb(OVERLAPPED *ovlp, NTSTATUS status, tudor_async_res_t res) {
    winmodule_set_cur(&tudor_adapter_dll->module);
    HRESULT hres;
    bool success = false, retry = false, found_match = false;

    if(status != STATUS_SUCCESS) {
        log_error("Error starting capture: 0x%x!", status);
        goto exit;
    }

    ULONG reject_detail;
    if((hres = tudor_sensor_adapter->FinishCapture(res->dev->pipeline, &reject_detail)) != ERROR_SUCCESS) {
        if(hres == WINBIO_E_BAD_CAPTURE) retry = true;
        log_error("Error finishing sensor capture: 0x%x! [reject detail 0x%x]", hres, reject_detail);
        goto exit;
    };
    if((hres = tudor_sensor_adapter->PushDataToEngine(res->dev->pipeline, WINBIO_PURPOSE_IDENTIFY, 0, &reject_detail)) != ERROR_SUCCESS) {
        if(hres == WINBIO_E_BAD_CAPTURE) retry = true;
        log_error("Error pushing sensor data to engine: 0x%x! [reject detail 0x%x]", hres, reject_detail);
        goto exit;
    };

    log_debug("Identifying sample...");
    WINBIO_IDENTITY identity;
    UCHAR subfactor;
    UCHAR *payload_ptr, *hash_ptr;
    SIZE_T payload_size, hash_size;
    if((hres = tudor_engine_adapter->IdentifyFeatureSet(res->dev->pipeline, &identity, &subfactor, &payload_ptr, &payload_size, &hash_ptr, &hash_size, &reject_detail)) != ERROR_SUCCESS) {
        if(hres == WINBIO_E_BAD_CAPTURE) retry = true;
        if(hres == WINBIO_E_UNKNOWN_ID) {
            success = true;
            found_match = false;
            goto exit;
        }

        log_error("Error identifying sample: 0x%x! [reject detail 0x%x]", hres, reject_detail);
        goto exit;
    };
    if(identity.Type != WINBIO_ID_TYPE_GUID) {
        log_error("Invalid identification ID type: %d!", identity.Type);
        goto exit;
    }

    success = true;
    found_match = true;
    *(res->args.identify.guid) = *(RECGUID*) &identity.TemplateGuid;
    *(res->args.identify.finger) = (enum tudor_finger) subfactor;

    exit:;
    *(res->args.identify.retry) = retry;
    *(res->args.identify.found_match) = found_match;
    async_complete_op(res, success);
}

bool tudor_identify(struct tudor_device *device, bool *retry, bool *found_match, RECGUID *guid, enum tudor_finger *finger, tudor_async_res_t *res) {
    winmodule_set_cur(&tudor_adapter_dll->module);
    *res = NULL;
    HRESULT hres;

    *retry = false;
    *found_match = false;
    if(device->enrolling) {
        log_error("Currently enrolling a finger!");
        return false;
    }

    log_debug("Capturing sample...");
    OVERLAPPED *ovlp;
    WINBIO_CALL_PIPELINE(tudor_sensor_adapter->StartCapture, device->pipeline, WINBIO_PURPOSE_IDENTIFY, &ovlp);

    *res = async_new_res(device, ovlp);
    (*res)->args.identify = (struct async_args_identify) { .retry = retry, .found_match = found_match, .guid = guid, .finger = finger };
    winio_set_overlapped_callback(ovlp, (winio_overlapped_cb_fnc*) identify_cb, *res, true);
    return true;
}