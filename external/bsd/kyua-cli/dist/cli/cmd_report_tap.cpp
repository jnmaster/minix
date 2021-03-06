// Copyright 2011 Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// * Redistributions of source code must retain the above copyright
//   notice, this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in the
//   documentation and/or other materials provided with the distribution.
// * Neither the name of Google Inc. nor the names of its contributors
//   may be used to endorse or promote products derived from this software
//   without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "cli/cmd_report.hpp"
#include "cli/cmd_report_tap.hpp"

#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <map>
#include <vector>

#include "cli/common.ipp"
#include "engine/action.hpp"
#include "engine/context.hpp"
#include "engine/drivers/scan_action.hpp"
#include "engine/test_result.hpp"
#include "utils/cmdline/exceptions.hpp"
#include "utils/cmdline/parser.ipp"
#include "utils/defs.hpp"
#include "utils/format/macros.hpp"
#include "utils/optional.ipp"

namespace cmdline = utils::cmdline;
namespace config = utils::config;
namespace datetime = utils::datetime;
namespace fs = utils::fs;
namespace scan_action = engine::drivers::scan_action;

using cli::cmd_report_tap;
using utils::optional;


namespace {


/// Generates a plain-text report intended to be printed to the console.
class console_tap_hooks : public scan_action::base_hooks {
    /// Indirection to print the output to the correct file stream.
    cli::file_writer _writer;

    /// Whether to include the runtime context in the output or not.
    const bool _show_context;

    /// Collection of result types to include in the report.
    const cli::result_types& _results_filters;

    /// The action ID loaded.
    int64_t _action_id;

    /// The total run time of the tests.
    datetime::delta _runtime;

    /// Representation of a single result.
    struct result_data {
        /// The relative path to the test program.
        fs::path binary_path;

        /// The name of the test case.
        std::string test_case_name;

        /// The result of the test case.
        engine::test_result result;

        /// The duration of the test case execution.
        datetime::delta duration;

        /// Constructs a new results data.
        ///
        /// \param binary_path_ The relative path to the test program.
        /// \param test_case_name_ The name of the test case.
        /// \param result_ The result of the test case.
        /// \param duration_ The duration of the test case execution.
        result_data(const fs::path& binary_path_,
                    const std::string& test_case_name_,
                    const engine::test_result& result_,
                    const datetime::delta& duration_) :
            binary_path(binary_path_), test_case_name(test_case_name_),
            result(result_), duration(duration_)
        {
        }
    };

    /// Results received, broken down by their type.
    ///
    /// Note that this may not include all results, as keeping the whole list in
    /// memory may be too much.
    std::map< engine::test_result::result_type,
              std::vector< result_data > > _results;

    /// Prints the execution context to the output.
    ///
    /// \param context The context to dump.
    void
    print_context(const engine::context& context)
    {
        _writer("  ===> Execution context");

        _writer(F("  Current directory: %s") % context.cwd());
        const std::map< std::string, std::string >& env = context.env();
        if (env.empty())
            _writer("  No environment variables recorded");
        else {
            _writer("  Environment variables:");
            for (std::map< std::string, std::string >::const_iterator
                     iter = env.begin(); iter != env.end(); iter++) {
                _writer(F("    %s=%s") % (*iter).first % (*iter).second);
            }
        }
    }

    /// Counts how many results of a given type have been received.
    std::size_t
    count_results(const engine::test_result::result_type type)
    {
        const std::map< engine::test_result::result_type,
                        std::vector< result_data > >::const_iterator iter =
            _results.find(type);
        if (iter == _results.end())
            return 0;
        else
            return (*iter).second.size();
    }

    /// Prints a set of results.
    void
    print_results(const engine::test_result::result_type type,
                  const char* title)
    {
        const std::map< engine::test_result::result_type,
                        std::vector< result_data > >::const_iterator iter2 =
            _results.find(type);
        if (iter2 == _results.end())
            return;
        const std::vector< result_data >& all = (*iter2).second;

	std::string result = "";
	std::string comment = "";
	
	if (type == engine::test_result::failed) 
	   result = "not ";

	if (type == engine::test_result::broken) {
	   result = "not ";
	   comment = " # broken";
	}

	if (type == engine::test_result::skipped) 
	   comment = " # skip";

        _writer(F("  ===> %s") % title);
        for (std::vector< result_data >::const_iterator iter = all.begin();
             iter != all.end(); iter++) {
            _writer(F("%sok %s:%s%s") %
	       	result %
	       	(*iter).binary_path %
	       	(*iter).test_case_name %
		comment
		);
            _writer(F("  %s:%s  ->  %s  [%s]") % (*iter).binary_path %
                    (*iter).test_case_name %
                    cli::format_result((*iter).result) %
                    cli::format_delta((*iter).duration));
        }
    }

public:
    /// Constructor for the hooks.
    ///
    /// \param ui_ The user interface object of the caller command.
    /// \param outfile_ The file to which to send the output.
    /// \param show_context_ Whether to include the runtime context in
    ///     the output or not.
    /// \param results_filters_ The result types to include in the report.
    ///     Cannot be empty.
    console_tap_hooks(cmdline::ui* ui_, const fs::path& outfile_,
                  const bool show_context_,
                  const cli::result_types& results_filters_) :
        _writer(ui_, outfile_),
        _show_context(show_context_),
        _results_filters(results_filters_)
    {
        PRE(!results_filters_.empty());
    }

    /// Callback executed when an action is found.
    ///
    /// \param action_id The identifier of the loaded action.
    /// \param action The action loaded from the database.
    void
    got_action(const int64_t action_id, const engine::action& action)
    {
        _action_id = action_id;
        if (_show_context)
            print_context(action.runtime_context());
    }

    /// Callback executed when a test results is found.
    ///
    /// \param iter Container for the test result's data.
    void
    got_result(store::results_iterator& iter)
    {
        _runtime += iter.duration();
        const engine::test_result result = iter.result();
        _results[result.type()].push_back(
            result_data(iter.test_program()->relative_path(),
                        iter.test_case_name(), iter.result(), iter.duration()));
    }

    /// Prints the tests summary.
    void
    print_tests(void)
    {
        using engine::test_result;
        typedef std::map< test_result::result_type, const char* > types_map;
        typedef std::map< test_result::result_type, std::size_t > types_counts;

        types_map titles;
        titles[engine::test_result::broken] = "Broken tests";
        titles[engine::test_result::expected_failure] = "Expected failures";
        titles[engine::test_result::failed] = "Failed tests";
        titles[engine::test_result::passed] = "Passed tests";
        titles[engine::test_result::skipped] = "Skipped tests";

	types_counts counts;
        counts[engine::test_result::broken] = count_results(test_result::broken);
        counts[engine::test_result::expected_failure] = count_results(test_result::expected_failure);
        counts[engine::test_result::failed] = count_results(test_result::failed);
        counts[engine::test_result::passed] = count_results(test_result::passed);
        counts[engine::test_result::skipped] = count_results(test_result::skipped);

        const std::size_t total = 
	    counts[engine::test_result::broken] +
	    counts[engine::test_result::expected_failure] +
	    counts[engine::test_result::failed] +
	    counts[engine::test_result::passed] +
	    counts[engine::test_result::skipped];

	std::size_t selected_types_total = 0;

        for (cli::result_types::const_iterator iter = _results_filters.begin();
             iter != _results_filters.end(); ++iter) {
	    selected_types_total += counts.find(*iter)->second;
	}

        _writer(F("1..%s") % selected_types_total);
        for (cli::result_types::const_iterator iter = _results_filters.begin();
             iter != _results_filters.end(); ++iter) {
            const types_map::const_iterator match = titles.find(*iter);
            INV_MSG(match != titles.end(), "Conditional does not match user "
                    "input validation in parse_types()");
            print_results((*match).first, (*match).second);
        }

        _writer("  ===> Summary");
        _writer(F("  Action: %s") % _action_id);
        _writer(F("  Test cases: %s total, %s passed, %s skipped, %s expected failures, "
                  "%s broken, %s failed") % total %
		counts[engine::test_result::passed] %
		counts[engine::test_result::skipped] %
		counts[engine::test_result::expected_failure] %
		counts[engine::test_result::broken] %
		counts[engine::test_result::failed]);
        _writer(F("  Total time: %s") % cli::format_delta(_runtime));
    }
};


}  // anonymous namespace


/// Default constructor for cmd_report_tap.
cmd_report_tap::cmd_report_tap(void) : cli_command(
    "report-tap", "", 0, 0,
    "Generates a TAP report with the result of a "
    "previous action")
{
    add_option(store_option);
    add_option(cmdline::bool_option(
        "show-context", "Include the execution context in the report"));
    add_option(cmdline::int_option(
        "action", "The action to report; if not specified, defaults to the "
        "latest action in the database", "id"));
    add_option(cmdline::path_option(
        "output", "The file to which to write the report",
        "path", "/dev/stdout"));
    add_option(results_filter_option);
}


/// Entry point for the "report" subcommand.
///
/// \param ui Object to interact with the I/O of the program.
/// \param cmdline Representation of the command line to the subcommand.
/// \param unused_user_config The runtime configuration of the program.
///
/// \return 0 if everything is OK, 1 if the statement is invalid or if there is
/// any other problem.
int
cmd_report_tap::run(cmdline::ui* ui, const cmdline::parsed_cmdline& cmdline,
                const config::tree& UTILS_UNUSED_PARAM(user_config))
{
    optional< int64_t > action_id;
    if (cmdline.has_option("action"))
        action_id = cmdline.get_option< cmdline::int_option >("action");

    const result_types types = get_result_types(cmdline);
    console_tap_hooks hooks(
        ui, cmdline.get_option< cmdline::path_option >("output"),
        cmdline.has_option("show-context"), types);
    scan_action::drive(store_path(cmdline), action_id, hooks);
    hooks.print_tests();

    return EXIT_SUCCESS;
}
