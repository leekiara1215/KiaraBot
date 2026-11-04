# STR07 Runtime Model streaming slice: strict stream consumption + Bridge stream dispatch.
# Included from cmake/platform.cmake after the runtime RPC/model/bridge targets exist; created by
# the streaming work stream, the include itself is owned by the lead.
target_link_libraries(kiarabot_runtime_model_client PUBLIC kiarabot_model_stream_codec)
target_link_libraries(kiarabot_runtime_rpc PUBLIC kiarabot_model_stream_codec)
