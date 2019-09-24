/*
 *
 * Copyright 2018 Asylo authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "asylo/identity/sgx/proto_format.h"

#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "absl/strings/escaping.h"
#include "asylo/identity/sgx/code_identity_util.h"
#include "asylo/identity/sgx/miscselect.pb.h"
#include "asylo/identity/sgx/platform_provisioning.pb.h"
#include "asylo/identity/sgx/secs_attributes.h"
#include "asylo/identity/sgx/secs_miscselect.h"
#include "asylo/identity/sgx/sgx_identity.pb.h"
#include "asylo/identity/util/sha256_hash.pb.h"

namespace asylo {
namespace sgx {
namespace {

using ::testing::HasSubstr;
using ::testing::Test;

constexpr char kValidCpuSvnHexString[] = "00112233445566778899aabbccddeeff";

TEST(ProtoFormatTest, SgxIdentityHasAttributesByName) {
  SgxIdentity identity;
  SetSelfSgxIdentity(&identity);
  std::string text = FormatProto(identity);

  std::vector<absl::string_view> named_attributes;
  GetPrintableAttributeList(identity.code_identity().attributes(),
                            &named_attributes);
  for (const auto attribute : named_attributes) {
    EXPECT_THAT(text, HasSubstr(std::string(attribute)));
  }
}

TEST(ProtoFormatTest, SgxIdentityHasCpuSvnAsHexString) {
  SgxIdentity sgx_identity;
  SetSelfSgxIdentity(&sgx_identity);

  CpuSvn cpu_svn;
  cpu_svn.set_value(absl::HexStringToBytes(kValidCpuSvnHexString));
  *sgx_identity.mutable_machine_configuration()->mutable_cpu_svn() = cpu_svn;
  ASSERT_TRUE(IsValidSgxIdentity(sgx_identity));

  std::string text = FormatProto(sgx_identity);
  EXPECT_THAT(text, HasSubstr(absl::StrCat("0x", kValidCpuSvnHexString)));
}

TEST(ProtoFormatTest, MiscselectBitsByName) {
  Miscselect miscselect;
  miscselect.set_value(UINT32_C(1)
                       << static_cast<size_t>(SecsMiscselectBit::EXINFO));
  std::string text = FormatProto(miscselect);

  std::vector<absl::string_view> named_miscselect_bits =
      GetPrintableMiscselectList(miscselect);
  for (const auto miscselect_bit : named_miscselect_bits) {
    EXPECT_THAT(text, HasSubstr(std::string(miscselect_bit)));
  }
}

TEST(ProtoFormatTest, SgxIdentityHasMiscselectBitsByName) {
  SgxIdentity identity;
  SetSelfSgxIdentity(&identity);
  std::string text = FormatProto(identity);

  std::vector<absl::string_view> named_miscselect_bits =
      GetPrintableMiscselectList(identity.code_identity().miscselect());
  for (const auto miscselect_bit : named_miscselect_bits) {
    EXPECT_THAT(text, HasSubstr(std::string(miscselect_bit)));
  }
}

TEST(ProtoFormatTest, SgxIdentityHasHexEncodedBytesFields) {
  SgxIdentity identity;
  SetSelfSgxIdentity(&identity);
  std::string text = FormatProto(identity);

  EXPECT_THAT(text,
              HasSubstr(absl::StrCat(
                  "0x", absl::BytesToHexString(
                            identity.code_identity().mrenclave().hash()))));
  EXPECT_THAT(text,
              HasSubstr(absl::StrCat(
                  "0x", absl::BytesToHexString(identity.code_identity()
                                                   .signer_assigned_identity()
                                                   .mrsigner()
                                                   .hash()))));
}

TEST(ProtoFormatTest, SgxIdentityMatchSpecHasAttributesByName) {
  SgxIdentityMatchSpec match_spec;
  SetDefaultMatchSpec(&match_spec);
  std::string text = FormatProto(match_spec);

  std::vector<absl::string_view> named_attributes;
  GetPrintableAttributeList(
      match_spec.code_identity_match_spec().attributes_match_mask(),
      &named_attributes);
  for (const auto attribute : named_attributes) {
    EXPECT_THAT(text, HasSubstr(std::string(attribute)));
  }
}

TEST(ProtoFormatTest, SgxIdentityMatchSpecHasMiscselectBitsByName) {
  SgxIdentityMatchSpec match_spec;
  SetDefaultMatchSpec(&match_spec);
  std::string text = FormatProto(match_spec);

  std::vector<absl::string_view> named_miscselect_bits =
      GetPrintableMiscselectList(
          match_spec.code_identity_match_spec().miscselect_match_mask());
  for (const auto miscselect_bit : named_miscselect_bits) {
    EXPECT_THAT(text, HasSubstr(std::string(miscselect_bit)));
  }
}

}  // namespace
}  // namespace sgx
}  // namespace asylo
