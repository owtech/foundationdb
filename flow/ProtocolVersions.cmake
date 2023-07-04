# Protocol Versions.
# This version impacts both communications and the deserialization of certain database and IKeyValueStore keys.
#
# The convention is that 'x' and 'y' should match the major and minor version of the software, and 'z' should be 0.
# To make a change without a corresponding increase to the x.y version, increment the 'dev' digit.
#
# The last 2 bytes (4 digits) of the protocol version do not affect compatibility. These two bytes are not currently
# used and should not be changed from 0.
#                                                          xyzdev
#                                                          vvvv
set(FDB_PV_DEFAULT_VERSION                      "0x0FDB00B074000000LL")
set(FDB_PV_FUTURE_VERSION                       "0x0FDB00B075000000LL")
set(FDB_PV_MIN_COMPATIBLE_VERSION               "0x0FDB00B073000000LL")
set(FDB_PV_MIN_INVALID_VERSION                  "0x0FDB00B076000000LL")
set(FDB_PV_LEFT_MOST_CHECK                      "0x0FDB00B100000000LL")
set(FDB_PV_LSB_MASK                             "0xF0FFFFLL")

# The 5th digit from right is dev version, for example, 2 in 0x0FDB00B061020000LL;
# It was used to identify a protocol change (e.g., interface change) between major/minor versions (say 5.1 and 5.2)
# We stopped using the dev version consistently in the past.
# To ensure binaries work across patch releases (e.g., 6.2.0 to 6.2.22), we require that the protocol version be
# the same for each of them.
set(FDB_PV_WATCHES                              "0x0FDB00A200090000LL")
set(FDB_PV_MOVABLE_COORDINATED_STATE            "0x0FDB00A2000D0000LL")
set(FDB_PV_PROCESS_ID                           "0x0FDB00A340000000LL")
set(FDB_PV_OPEN_DATABASE                        "0x0FDB00A400040000LL")
set(FDB_PV_LOCALITY                             "0x0FDB00A446020000LL")
set(FDB_PV_MULTIGENERATION_TLOG                 "0x0FDB00A460010000LL")
set(FDB_PV_SHARED_MUTATIONS                     "0x0FDB00A460010000LL")
set(FDB_PV_INEXPENSIVE_MULTIVERSION_CLIENT      "0x0FDB00A551000000LL")
set(FDB_PV_TAG_LOCALITY                         "0x0FDB00A560010000LL")
set(FDB_PV_FEARLESS                             "0x0FDB00B060000000LL")
set(FDB_PV_ENDPOINT_ADDR_LIST                   "0x0FDB00B061020000LL")
set(FDB_PV_IPV6                                 "0x0FDB00B061030000LL")
set(FDB_PV_TLOG_VERSION                         "0x0FDB00B061030000LL")
set(FDB_PV_PSEUDO_LOCALITIES                    "0x0FDB00B061070000LL")
set(FDB_PV_SHARDED_TXS_TAGS                     "0x0FDB00B061070000LL")
set(FDB_PV_TLOG_QUEUE_ENTRY_REF                 "0x0FDB00B062010001LL")
set(FDB_PV_GENERATION_REG_VAL                   "0x0FDB00B062010001LL")
set(FDB_PV_MOVABLE_COORDINATED_STATE_V2         "0x0FDB00B062010001LL")
set(FDB_PV_KEY_SERVER_VALUE                     "0x0FDB00B062010001LL")
set(FDB_PV_LOGS_VALUE                           "0x0FDB00B062010001LL")
set(FDB_PV_SERVER_TAG_VALUE                     "0x0FDB00B062010001LL")
set(FDB_PV_TAG_LOCALITY_LIST_VALUE              "0x0FDB00B062010001LL")
set(FDB_PV_DATACENTER_REPLICAS_VALUE            "0x0FDB00B062010001LL")
set(FDB_PV_PROCESS_CLASS_VALUE                  "0x0FDB00B062010001LL")
set(FDB_PV_WORKER_LIST_VALUE                    "0x0FDB00B062010001LL")
set(FDB_PV_BACKUP_START_VALUE                   "0x0FDB00B062010001LL")
set(FDB_PV_LOG_RANGE_ENCODE_VALUE               "0x0FDB00B062010001LL")
set(FDB_PV_HEALTHY_ZONE_VALUE                   "0x0FDB00B062010001LL")
set(FDB_PV_DR_BACKUP_RANGES                     "0x0FDB00B062010001LL")
set(FDB_PV_REGION_CONFIGURATION                 "0x0FDB00B062010001LL")
set(FDB_PV_REPLICATION_POLICY                   "0x0FDB00B062010001LL")
set(FDB_PV_BACKUP_MUTATIONS                     "0x0FDB00B062010001LL")
set(FDB_PV_CLUSTER_CONTROLLER_PRIORITY_INFO     "0x0FDB00B062010001LL")
set(FDB_PV_PROCESS_ID_FILE                      "0x0FDB00B062010001LL")
set(FDB_PV_CLOSE_UNUSED_CONNECTION              "0x0FDB00B062010001LL")
set(FDB_PV_DB_CORE_STATE                        "0x0FDB00B063010000LL")
set(FDB_PV_TAG_THROTTLE_VALUE                   "0x0FDB00B063010000LL")
set(FDB_PV_STORAGE_CACHE_VALUE                  "0x0FDB00B063010000LL")
set(FDB_PV_RESTORE_STATUS_VALUE                 "0x0FDB00B063010000LL")
set(FDB_PV_RESTORE_REQUEST_VALUE                "0x0FDB00B063010000LL")
set(FDB_PV_RESTORE_REQUEST_DONE_VERSION_VALUE   "0x0FDB00B063010000LL")
set(FDB_PV_RESTORE_REQUEST_TRIGGER_VALUE        "0x0FDB00B063010000LL")
set(FDB_PV_RESTORE_WORKER_INTERFACE_VALUE       "0x0FDB00B063010000LL")
set(FDB_PV_BACKUP_PROGRESS_VALUE                "0x0FDB00B063010000LL")
set(FDB_PV_KEY_SERVER_VALUE_V2                  "0x0FDB00B063010000LL")
set(FDB_PV_UNIFIED_TLOG_SPILLING                "0x0FDB00B063000000LL")
set(FDB_PV_BACKUP_WORKER                        "0x0FDB00B063010000LL")
set(FDB_PV_REPORT_CONFLICTING_KEYS              "0x0FDB00B063010000LL")
set(FDB_PV_SMALL_ENDPOINTS                      "0x0FDB00B063010000LL")
set(FDB_PV_CACHE_ROLE                           "0x0FDB00B063010000LL")
set(FDB_PV_STABLE_INTERFACES                    "0x0FDB00B070010000LL")
set(FDB_PV_SERVER_LIST_VALUE                    "0x0FDB00B070010001LL")
set(FDB_PV_TAG_THROTTLE_VALUE_REASON            "0x0FDB00B070010001LL")
set(FDB_PV_SPAN_CONTEXT                         "0x0FDB00B070010001LL")
set(FDB_PV_TSS                                  "0x0FDB00B070010001LL")
set(FDB_PV_CHANGE_FEED                          "0x0FDB00B071010000LL")
set(FDB_PV_BLOB_GRANULE                         "0x0FDB00B071010000LL")
set(FDB_PV_NETWORK_ADDRESS_HOSTNAME_FLAG        "0x0FDB00B071010000LL")
set(FDB_PV_STORAGE_METADATA                     "0x0FDB00B071010000LL")
set(FDB_PV_PERPETUAL_WIGGLE_METADATA            "0x0FDB00B071010000LL")
set(FDB_PV_STORAGE_INTERFACE_READINESS          "0x0FDB00B071010000LL")
set(FDB_PV_TENANTS                              "0x0FDB00B071010000LL")
set(FDB_PV_RESOLVER_PRIVATE_MUTATIONS           "0x0FDB00B071010000LL")
set(FDB_PV_OTEL_SPAN_CONTEXT                    "0x0FDB00B072000000LL")
set(FDB_PV_SW_VERSION_TRACKING                  "0x0FDB00B072000000LL")
set(FDB_PV_ENCRYPTION_AT_REST                   "0x0FDB00B072000000LL")
set(FDB_PV_SHARD_ENCODE_LOCATION_METADATA       "0x0FDB00B072000000LL")
set(FDB_PV_BLOB_GRANULE_FILE                    "0x0FDB00B072000000LL")
set(FDB_ENCRYPTED_SNAPSHOT_BACKUP_FILE          "0x0FDB00B072000000LL")
set(FDB_PV_CLUSTER_ID_SPECIAL_KEY               "0x0FDB00B072000000LL")
set(FDB_PV_BLOB_GRANULE_FILE_LOGICAL_SIZE       "0x0FDB00B072000000LL")
set(FDB_PV_BLOB_RANGE_CHANGE_LOG                "0x0FDB00B072000000LL")
set(FDB_PV_GC_TXN_GENERATIONS                   "0x0FDB00B073000000LL")
