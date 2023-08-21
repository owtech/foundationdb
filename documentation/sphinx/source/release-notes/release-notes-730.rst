#############
Release Notes
#############

7.3.0
=====

Fixes
-----
* Fixed a consistency scan infinite looping without progress bug when a storage server is removed. `(PR #9154) <https://github.com/apple/foundationdb/pull/9154>`_
* Fixed a backup worker assertion failure. `(PR #8886) <https://github.com/apple/foundationdb/pull/8886>`_
* Fixed a DD stuck issue when the remote data center is dead. `(PR #9338) <https://github.com/apple/foundationdb/pull/9338>`_
* Exclude command will not perform a write if the addresses being excluded are already excluded. `(PR #9873) <https://github.com/apple/foundationdb/pull/9873>`_
* ConsistencyCheck should finish after complete scan than failing on first mismatch. `(PR #8539) <https://github.com/apple/foundationdb/pull/8539>`_

Bindings
--------
* Allow Ruby bindings to run on arm64. `(PR #9575) <https://github.com/apple/foundationdb/pull/9575>`_

Performance
-----------
* Improvements on physical shard creation to reduce shard count. `(PR #9067) <https://github.com/apple/foundationdb/pull/9067>`_
* Older TLog generations are garbage collected as soon as they are no longer needed. `(PR #10289) <https://github.com/apple/foundationdb/pull/10289>`_

Reliability
-----------
* Gray failure will monitor satellite TLog disconnections.
* Storage progress is logged during the slow recovery. `(PR #9041) <https://github.com/apple/foundationdb/pull/9041>`_
* Added a new network option fail_incompatible_client. If the option is set, transactions are failing with fail_incompatible_client in case of an attempt to connect to a cluster without providing a compatible client library

Status
------

Other Changes
-------------

*  Added MonotonicTime field, based on system clock, to CommitDebug
   trace events, for accurate timing.

*  Added a new function fdb_database_get_client_status providing a
   client-side connection status information in json format.

*  Added a new network option retain_client_library_copies to avoid
   deleting the temporary library copies after completion of the
   process. This may be useful in various debugging and profiling
   scenarios.

*  Added a new network option trace_initialize_on_setup to enable client
   traces already on fdb_setup_network, so that traces do not get lost
   on client configuration issues

*  TraceEvents related to TLS handshake, new connections, and tenant
   access by authorization token are no longer subject to suppression or
   throttling, using an internal “AuditedEvent” TraceEvent
   classification

*  Usage of authorization token is logged as part of AuditedEvent, with
   5-second suppression time window for duplicate entries (suppression
   time window is controlled by AUDIT_TIME_WINDOW flow knob)

Earlier release notes
---------------------
* :doc:`7.2 (API Version 720) </release-notes/release-notes-720>`
* :doc:`7.1 (API Version 710) </release-notes/release-notes-710>`
* :doc:`7.0 (API Version 700) </release-notes/release-notes-700>`
* :doc:`6.3 (API Version 630) </release-notes/release-notes-630>`
* :doc:`6.2 (API Version 620) </release-notes/release-notes-620>`
* :doc:`6.1 (API Version 610) </release-notes/release-notes-610>`
* :doc:`6.0 (API Version 600) </release-notes/release-notes-600>`
* :doc:`5.2 (API Version 520) </release-notes/release-notes-520>`
* :doc:`5.1 (API Version 510) </release-notes/release-notes-510>`
* :doc:`5.0 (API Version 500) </release-notes/release-notes-500>`
* :doc:`4.6 (API Version 460) </release-notes/release-notes-460>`
* :doc:`4.5 (API Version 450) </release-notes/release-notes-450>`
* :doc:`4.4 (API Version 440) </release-notes/release-notes-440>`
* :doc:`4.3 (API Version 430) </release-notes/release-notes-430>`
* :doc:`4.2 (API Version 420) </release-notes/release-notes-420>`
* :doc:`4.1 (API Version 410) </release-notes/release-notes-410>`
* :doc:`4.0 (API Version 400) </release-notes/release-notes-400>`
* :doc:`3.0 (API Version 300) </release-notes/release-notes-300>`
* :doc:`2.0 (API Version 200) </release-notes/release-notes-200>`
* :doc:`1.0 (API Version 100) </release-notes/release-notes-100>`
* :doc:`Beta 3 (API Version 23) </release-notes/release-notes-023>`
* :doc:`Beta 2 (API Version 22) </release-notes/release-notes-022>`
* :doc:`Beta 1 (API Version 21) </release-notes/release-notes-021>`
* :doc:`Alpha 6 (API Version 16) </release-notes/release-notes-016>`
* :doc:`Alpha 5 (API Version 14) </release-notes/release-notes-014>`
