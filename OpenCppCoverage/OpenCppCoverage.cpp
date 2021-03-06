// OpenCppCoverage is an open source code coverage for C++.
// Copyright (C) 2014 OpenCppCoverage
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "stdafx.h"
#include "OpenCppCoverage.hpp"

#include <iostream>

#include "CppCoverage/CodeCoverageRunner.hpp"
#include "CppCoverage/CoverageFilterSettings.hpp"
#include "CppCoverage/OptionsParser.hpp"
#include "CppCoverage/Options.hpp"
#include "CppCoverage/ProgramOptions.hpp"
#include "CppCoverage/CoverageDataMerger.hpp"
#include "CppCoverage/OptionsExport.hpp"
#include "CppCoverage/RunCoverageSettings.hpp"

#include "Exporter/Html/HtmlExporter.hpp"
#include "Exporter/CoberturaExporter.hpp"
#include "Exporter/Binary/BinaryExporter.hpp"
#include "Exporter/Binary/CoverageDataDeserializer.hpp"

#include "Tools/Tool.hpp"
#include "Tools/Log.hpp"
#include "Tools/WarningManager.hpp"

namespace cov = CppCoverage;
namespace logging = boost::log;

namespace OpenCppCoverage
{
	namespace
	{
		//-----------------------------------------------------------------------------
		std::wstring GetDefaultPathPrefix(const cov::Options& options)
		{
			const auto* startInfo = options.GetStartInfo();

			if (startInfo)
			{
				auto path = startInfo->GetPath();
				fs::path runningCommandFilenamePath = path.filename().replace_extension("");
				
				return runningCommandFilenamePath.wstring();
			}

			return L"CoverageOutput";
		}

		//-----------------------------------------------------------------------------
		void Export(
			const cov::Options& options, 
			const cov::CoverageData& coverage)
		{
			const auto& exports = options.GetExports();
			std::map<cov::OptionsExportType, std::unique_ptr<Exporter::IExporter>> exporters;
			
			exporters.emplace(cov::OptionsExportType::Html, 
				std::unique_ptr<Exporter::IExporter>(new Exporter::HtmlExporter{ Tools::GetTemplateFolder() }));
			exporters.emplace(cov::OptionsExportType::Cobertura, 
				std::unique_ptr<Exporter::IExporter>(new Exporter::CoberturaExporter{}));
			exporters.emplace(cov::OptionsExportType::Binary,
				std::unique_ptr<Exporter::IExporter>(new Exporter::BinaryExporter{}));
			
			auto defaultPathPrefix = GetDefaultPathPrefix(options);

			for (const auto& singleExport : exports)
			{
				const auto& exporter = exporters.at(singleExport.GetType());
				auto optionalOutputPath = singleExport.GetOutputPath();
				auto output = (optionalOutputPath) ? *optionalOutputPath : exporter->GetDefaultPath(defaultPathPrefix);

				exporter->Export(coverage, output);
			}
		}

		//-----------------------------------------------------------------------------
		std::vector<cov::CoverageData> LoadInputCoverageDatas(const cov::Options& options)
		{
			std::vector<cov::CoverageData> coverageDatas;
			Exporter::CoverageDataDeserializer coverageDataDeserializer;

			for (const auto& path : options.GetInputCoveragePaths())
			{				
				auto errorMsg = "Cannot extract coverage data from " + path.string();

				LOG_INFO << L"Load coverage file: " << path.wstring();
				coverageDatas.push_back(coverageDataDeserializer.Deserialize(path, errorMsg));
			}
			return coverageDatas;
		}

		//-----------------------------------------------------------------------------
		void InitLogger(const cov::Options& options)
		{
			auto logLevel = logging::trivial::info;

			switch (options.GetLogLevel())
			{
				case cov::LogLevel::Verbose: logLevel = logging::trivial::debug; break;
				case cov::LogLevel::Quiet: logLevel = logging::trivial::error; break;
			}

			Tools::InitConsoleAndFileLog(L"LastCoverageResults.log");
			Tools::SetLoggerMinSeverity(logLevel);
		}

		//-----------------------------------------------------------------------------
		int Run(const cov::Options& options)
		{
			InitLogger(options);

			auto coveraDatas = LoadInputCoverageDatas(options);
			const auto* startInfo = options.GetStartInfo();
			
			std::wostringstream ostr;
			ostr << std::endl << options;
			LOG_INFO << L"Start Program:" << ostr.str();

			cov::CodeCoverageRunner codeCoverageRunner;
			cov::CoverageFilterSettings coverageFilterSettings{ options.GetModulePatterns(), options.GetSourcePatterns() };
			auto exitCode = 0;

			if (startInfo)
			{
				size_t maxUnmatchPathsForWarning = (options.GetLogLevel() == cov::LogLevel::Verbose) 
					? std::numeric_limits<size_t>::max() : 30;

				cov::RunCoverageSettings runCoverageSettings(
				    *startInfo,
				    coverageFilterSettings,
				    options.GetUnifiedDiffSettingsCollection(),
				    options.GetExcludedLineRegexes(),
				    options.GetSubstitutePdbSourcePaths());

				runCoverageSettings.SetCoverChildren(options.IsCoverChildrenModeEnabled());
				runCoverageSettings.SetContinueAfterCppException(options.IsContinueAfterCppExceptionModeEnabled());
				runCoverageSettings.SetMaxUnmatchPathsForWarning(maxUnmatchPathsForWarning);
				runCoverageSettings.SetOptimizedBuildSupport(options.IsOptimizedBuildSupportEnabled());
				auto coverageData = codeCoverageRunner.RunCoverage(runCoverageSettings);
				exitCode = coverageData.GetExitCode();
				coveraDatas.push_back(std::move(coverageData));
			}
			cov::CoverageDataMerger	coverageDataMerger;

			auto coverageData = coverageDataMerger.Merge(coveraDatas);

			if (options.IsAggregateByFileModeEnabled())
				coverageDataMerger.MergeFileCoverage(coverageData);

			Export(options, coverageData);

			if (exitCode)
				LOG_ERROR << L"Your program stop with error code: " << exitCode;
			return exitCode;
		}
	}

	//-----------------------------------------------------------------------------
	int OpenCppCoverage::Run(int argc,
	                         const char** argv,
	                         std::wostream* emptyOptionsExplanation) const
	{
		auto warningManager = std::make_shared<Tools::WarningManager>();
		cov::OptionsParser optionsParser{warningManager};

		auto options = optionsParser.Parse(argc, argv, emptyOptionsExplanation);
		auto status = FailureExitCode;

		if (options)
		{
			try
			{
				status = ::OpenCppCoverage::Run(*options);
			}
			catch (const std::exception& e)
			{
				LOG_ERROR << "Error: " << e.what();
			}
			catch (...)
			{
				LOG_ERROR << "Unkown Error";
			}

			warningManager->DisplayWarnings();
			if (options->IsPlugingModeEnabled())
			{
				std::cout << "Press any key to continue... ";
				std::cin.get();
			}
		}

		return status;
	}
}