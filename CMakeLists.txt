cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(starz_bms)

target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/drivers/bms_monitor/bq76952
  ${CMAKE_CURRENT_SOURCE_DIR}/platform/stm32
)

target_sources(app PRIVATE

  subsys/bms/bms_monitor.c
  subsys/bms/bms_topology.c
  subsys/bms/bms_measurement.c
  subsys/bms/bms_events.c
  subsys/bms/bms_estimator.c
  subsys/bms/bms_config.c
  subsys/bms/bms_config_store.c
  subsys/bms/bms_protocol.c
  subsys/bms/bms_communications.c
  subsys/bms/bms_system_policy.c
  subsys/bms/bms_system.c
  subsys/bms/bms_protection.c
  subsys/bms/bms_balancing.c
  subsys/bms/bms_state_machine.c
  subsys/bms/bms_core_runtime.c
  drivers/bms_monitor/bq76952/bq76952.c
  drivers/bms_monitor/bq76952/bms_monitor_bq76952.c
)

if(CONFIG_BMS_PLATFORM_NATIVE_SIM)
  target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/virtual_bq)
  target_sources(app PRIVATE
    src/main_native.c
    subsys/bms/bms_config_store_virtual.c
    subsys/bms/bms_communications_virtual.c
    subsys/bms/bms_watchdog_virtual.c
    tests/virtual_bq/virtual_bq_model.c
    tests/virtual_bq/bq76952_transport_virtual.c
  )
endif()

if(CONFIG_BMS_PLATFORM_STM32F446)
  target_sources(app PRIVATE
    src/main_stm32.c
    drivers/bms_monitor/bq76952/bq76952_transport_i2c.c
    platform/stm32/bms_board_stm32.c
    platform/stm32/bms_communications_stm32.c
  )

  if(CONFIG_BMS_STM32_USE_HW_WATCHDOG)
    target_sources(app PRIVATE platform/stm32/bms_watchdog_stm32.c)
  endif()

  if(CONFIG_BMS_STM32_USE_NVS)
    target_sources(app PRIVATE platform/stm32/bms_config_store_nvs.c)
  else()
    target_sources(app PRIVATE subsys/bms/bms_config_store_virtual.c)
  endif()
endif()
