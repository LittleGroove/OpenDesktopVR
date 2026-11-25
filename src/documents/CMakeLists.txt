# SPDX-License-Identifier: GPL-3.0-only
# Copyright (c) 2026 Robert McGough
#
# Documents as panel content (M8, doc 04 "Documents", ADR-047): PDFs through
# Windows.Data.Pdf, images through Windows' decoders, and folders of them;
# drawn on each panel's own thread into memory, shown by the render thread.

odvr_module(
  documents
  SOURCES
  DocumentFormats.cpp
  PageCache.cpp
  DocumentStatus.cpp
  Document.cpp
  DocumentSource.cpp
  DEPS
  odvr::render
  odvr::core
  odvr::cppwinrt
  d2d1
  dwrite
  PRIVATE_DEPS
  shlwapi
)
