# SPDX-License-Identifier: Apache-2.0
#
# corTestParams.sh - sourced by corTest before it parses its command line
#
# The broker under test: corTest's run header shows its --version.
#
export COR_BROKER="${AAS_BROKER:-coraine}"

#
# -basyx - the Eclipse BaSyx AAS environment aas_basyx runs against (# SKIP_BASYX: off):
#
#   http[s]://...  one already running (CI: the job's service, $AAS_BASYX_URL)
#   <image>        started by the test in docker, on 127.0.0.1:$AAS_BASYX_PORT (the default when docker answers)
#   off            the test is skipped (the default with no docker)
#
if [ -n "$AAS_BASYX_URL" ]; then
  basyxDefault="$AAS_BASYX_URL"
elif docker info > /dev/null 2>&1; then
  basyxDefault="eclipsebasyx/aas-environment:2.0.0-milestone-16"
else
  basyxDefault="off"
fi
corCliParamAdd "-basyx" "AAS_BASYX" "$basyxDefault" "Eclipse BaSyx for aas_basyx: a URL, an image to start in docker, or off" "BASYX"

#
# -faaast - FA³ST Service (Fraunhofer IOSB) aas_faaast runs against (# SKIP_FAAAST: off), as -basyx: a URL (CI's
# service, $AAS_FAAAST_URL - https, its API under /api/v3.0), an image started in docker, or off
#
if [ -n "$AAS_FAAAST_URL" ]; then
  faaastDefault="$AAS_FAAAST_URL"
elif docker info > /dev/null 2>&1; then
  faaastDefault="fraunhoferiosb/faaast-service:1.3.0"
else
  faaastDefault="off"
fi
corCliParamAdd "-faaast" "AAS_FAAAST" "$faaastDefault" "FA³ST Service for aas_faaast: a URL, an image to start in docker, or off" "FAAAST"
