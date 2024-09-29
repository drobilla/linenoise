# Copyright 2020-2024 David Robillard <d@drobilla.net>
# SPDX-License-Identifier: BSD-2-Clause

run_test_py = files('run_test.py')

# Unit Tests

test_history_sources = files('test_history.c')
test(
  'history',
  executable(
    'test_history',
    test_history_sources,
    c_args: c_suppressions,
    dependencies: comlin_dep,
    include_directories: include_dirs,
  ),
)

# Data-Driven Tests

test_comlin_sources = files('test_comlin.c')
test_comlin = executable(
  'test_comlin',
  test_comlin_sources,
  c_args: platform_c_args + c_suppressions,
  dependencies: comlin_dep,
  include_directories: include_dirs,
)

subdir('common')
subdir('dumb')
subdir('history')
subdir('mask')
subdir('multi')
subdir('single')

# Lint

if get_option('lint')
  test_sources = test_history_sources + test_comlin_sources
  all_sources = c_headers + sources + example_sources + test_sources

  # Check code formatting
  clang_format = find_program('clang-format', required: false)
  if clang_format.found()
    test(
      'format',
      clang_format,
      args: ['--Werror', '--dry-run'] + all_sources,
      suite: 'code',
    )
  endif
endif

