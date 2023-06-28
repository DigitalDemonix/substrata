#!/usr/bin/ruby

# This file defines configuration options to be used instead of hardcoded values.
# The version of Qt we use needs to be accessed by both CMake and ruby build_dist script etc..#

#require './script_utils.rb'

# The config options

$vs_version = 2022 # Visual studio option used to build distribution.  Used in build.rb

$libs_vs_version = 2019 # VS version used to build libraries - Qt etc.  Can be lower than $vs_version.
# NOTE: should match CYBERSPACE_LIBS_VS_VER in Cmakelists.txt


$qt_version = "5.13.2" if OS.windows?
#$qt_version = "6.2.2" if OS.windows?
$qt_version = "5.15.4" if OS.mac?
$qt_version = "5.13.2" if OS.linux?


$llvm_version = ""
if OS.windows?
	$llvm_version = "6.0.0" # LLVM 3.4 is the last version that builds in pre-vs 2015.
else
	$llvm_version = "6.0.0" # LLVM 3.4 fails to build on High Sierra, and 6.0.0 works fine.
end

# Get Qt path.
glare_core_libs_dir = ENV['GLARE_CORE_LIBS']
if glare_core_libs_dir.nil?
	puts "GLARE_CORE_LIBS env var not defined."
	exit(1)
end

indigo_qt_base_dir = "#{glare_core_libs_dir}/Qt"

$indigo_qt_dir = ""
if OS.unix?
	$indigo_qt_dir = "#{indigo_qt_base_dir}/#{$qt_version}"
else
	$indigo_qt_dir = "#{indigo_qt_base_dir}/#{$qt_version}-vs#{$vs_version}-64"
end


def get_substrata_version
	versionfile = IO.readlines("../shared/Version.h").join

	versionmatch = versionfile.match(/cyberspace_version\s+=\s+\"(.*)\"/)

	if versionmatch.nil? || versionmatch[1].nil?
		puts "Failed to extract version number from Version.h"
		exit(1)
	end

	version = versionmatch[1]
	version
end
