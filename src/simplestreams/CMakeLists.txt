# Copyright © 2017-2022 Canonical Ltd.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 3 as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
# Authored by: Chris Townsend <christopher.townsend@canonical.com>

add_library(simplestreams STATIC
            simple_streams_index.cpp
            simple_streams_manifest.cpp)

target_link_libraries(simplestreams
  network
  settings
  utils
  Qt5::Core
  Qt5::Network)
