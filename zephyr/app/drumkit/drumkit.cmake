# Copyright (c) 2025 Dato Musical Instruments
# SPDX-License-Identifier: Apache-2.0

# Synthesises the sampled drums with drumkit.py and adds the generated source
# to `target`. The firmware and the tests that check the samples both use it.
function(duo_drumkit_sources target)
  set(script ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/drumkit.py)
  set(out_dir ${CMAKE_CURRENT_BINARY_DIR}/drumkit)
  set(out ${out_dir}/drum_samples.cpp)

  add_custom_command(
    OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
    COMMAND ${PYTHON_EXECUTABLE} ${script} --out ${out}
    DEPENDS ${script}
    COMMENT "Synthesising the sampled drums"
    VERBATIM
  )

  target_sources(${target} PRIVATE ${out})
endfunction()
