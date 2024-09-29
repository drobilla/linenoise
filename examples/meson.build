# Copyright 2020-2023 David Robillard <d@drobilla.net>
# SPDX-License-Identifier: BSD-2-Clause

example_sources = files('example.c')

example = executable(
  'example',
  example_sources,
  c_args: c_suppressions,
  dependencies: comlin_dep,
  include_directories: include_dirs,
)

test('badarg', example, args: ['--badarg'], should_fail: true)
