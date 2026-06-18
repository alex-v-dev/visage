/* Copyright Vital Audio, LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include "defines.h"

#include <filesystem>
#include <vector>

namespace visage {
  typedef std::filesystem::path File;

  VISAGE_EXPORT bool replaceFileWithData(const File& file, const unsigned char* data, size_t size);
  VISAGE_EXPORT bool replaceFileWithText(const File& file, const std::string& text);
  VISAGE_EXPORT bool hasWriteAccess(const File& file);
  VISAGE_EXPORT bool fileExists(const File& file);
  VISAGE_EXPORT bool isDirectory(const File& file);
  VISAGE_EXPORT bool appendTextToFile(const File& file, const std::string& text);
  VISAGE_EXPORT std::unique_ptr<unsigned char[]> loadFileData(const File& file, size_t& size);
  VISAGE_EXPORT std::string loadFileAsString(const File& file);

  VISAGE_EXPORT File hostExecutable();
  VISAGE_EXPORT File appDataDirectory();
  VISAGE_EXPORT File userDocumentsDirectory();
  VISAGE_EXPORT File createTemporaryFile(const std::string& extension);
  VISAGE_EXPORT void createDirectory(const File& file);

  VISAGE_EXPORT std::string fileName(const File& file);
  VISAGE_EXPORT std::string fileStem(const File& file);
  VISAGE_EXPORT std::string hostName();
  VISAGE_EXPORT std::vector<File> searchForFiles(const File& directory, const std::string& regex);
  VISAGE_EXPORT std::vector<File> searchForDirectories(const File& directory, const std::string& regex);
}