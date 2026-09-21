// 命令行解析模块测试
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "libmini.h"

namespace {

libmini::Args make_args()
{
    libmini::Args args("demo", "1.0", "示例程序");
    args.add_option("host", "H", "服务器地址", std::string("127.0.0.1"));
    args.add_int("port", "p", "端口", 8080);
    args.add_double("ratio", "", "比例", 0.5);
    args.add_option("token", "t", "令牌");            // 必填（无默认值）
    args.add_flag("verbose", "v", "详细输出");
    args.add_positional("input", "输入文件");
    return args;
}

}  // namespace

TEST(ArgsTest, DefaultsWhenNotProvided)
{
    libmini::Args args = make_args();
    // 提供 token 与位置参数，但不提供 host/port
    const char* argv[] = {"demo", "--token", "abc", "in.txt"};
    ASSERT_TRUE(args.parse(4, const_cast<char**>(argv)));

    EXPECT_EQ(args.get_string("host"), "127.0.0.1");
    EXPECT_EQ(args.get_int("port"), 8080);
    EXPECT_DOUBLE_EQ(args.get_double("ratio"), 0.5);
    EXPECT_EQ(args.get_string("token"), "abc");
    EXPECT_FALSE(args.has_flag("verbose"));
    EXPECT_EQ(args.positional("input"), "in.txt");   // 位置参数已提供
}

TEST(ArgsTest, LongOptionEqualsAndSeparateValue)
{
    libmini::Args args = make_args();

    const char* a1[] = {"demo", "--host=10.0.0.1", "--token=x", "in.txt"};
    ASSERT_TRUE(args.parse(4, const_cast<char**>(a1)));
    EXPECT_EQ(args.get_string("host"), "10.0.0.1");
    EXPECT_EQ(args.get_string("token"), "x");

    const char* a2[] = {"demo", "--host", "localhost", "--token", "y", "in.txt"};
    ASSERT_TRUE(args.parse(6, const_cast<char**>(a2)));
    EXPECT_EQ(args.get_string("host"), "localhost");
    EXPECT_EQ(args.get_string("token"), "y");
}

TEST(ArgsTest, ShortOptionForms)
{
    libmini::Args args = make_args();

    const char* a1[] = {"demo", "-H", "1.2.3.4", "-p", "9999", "-t", "k", "in.txt"};
    ASSERT_TRUE(args.parse(8, const_cast<char**>(a1)));
    EXPECT_EQ(args.get_string("host"), "1.2.3.4");
    EXPECT_EQ(args.get_int("port"), 9999);

    // -p9999 与 -p=9999 附值形式
    const char* a2[] = {"demo", "-p9999", "-t=k", "-H2.2.2.2", "in.txt"};
    ASSERT_TRUE(args.parse(5, const_cast<char**>(a2)));
    EXPECT_EQ(args.get_int("port"), 9999);
    EXPECT_EQ(args.get_string("token"), "k");
    EXPECT_EQ(args.get_string("host"), "2.2.2.2");
}

TEST(ArgsTest, FlagsAndNegation)
{
    libmini::Args args = make_args();
    const char* argv[] = {"demo", "-v", "--verbose", "--token", "z", "in.txt"};
    ASSERT_TRUE(args.parse(6, const_cast<char**>(argv)));
    EXPECT_TRUE(args.has_flag("verbose"));   // 出现即 true
}

TEST(ArgsTest, StrictNumericValidation)
{
    libmini::Args args = make_args();

    const char* bad1[] = {"demo", "--port", "80x80", "--token", "t"};
    EXPECT_FALSE(args.parse(5, const_cast<char**>(bad1)));   // 非整数

    const char* bad2[] = {"demo", "--ratio", "abc", "--token", "t"};
    EXPECT_FALSE(args.parse(5, const_cast<char**>(bad2)));   // 非数字

    const char* ok[] = {"demo", "--port", "-5", "--ratio", "-1.5e2", "--token", "t",
                        "in.txt"};
    ASSERT_TRUE(args.parse(8, const_cast<char**>(ok)));      // 负数/科学计数法合法
    EXPECT_EQ(args.get_int("port"), -5);
    EXPECT_DOUBLE_EQ(args.get_double("ratio"), -150.0);
}

TEST(ArgsTest, PositionalAndRemaining)
{
    libmini::Args args("app", "", "");
    args.add_positional("input", "输入");
    args.add_flag("force", "f", "覆盖");

    const char* argv[] = {"app", "--force", "in.txt", "out1.txt", "out2.txt"};
    ASSERT_TRUE(args.parse(5, const_cast<char**>(argv)));
    EXPECT_EQ(args.positional("input"), "in.txt");
    const std::vector<std::string> rest = args.remaining();
    ASSERT_EQ(rest.size(), 2u);
    EXPECT_EQ(rest[0], "out1.txt");
    EXPECT_EQ(rest[1], "out2.txt");
    EXPECT_TRUE(args.has_flag("force"));

    // 缺位置参数报错
    const char* missing[] = {"app"};
    EXPECT_FALSE(args.parse(1, const_cast<char**>(missing)));
}

TEST(ArgsTest, DoubleDashStopsOptionParsing)
{
    // 用无必填位置参数的定义，让 "--" 后的内容全部归入 remaining
    libmini::Args args("app", "", "");
    args.add_flag("verbose", "v", "详细输出");
    args.add_option("token", "", "令牌");

    const std::vector<std::string> v = {"--", "--token", "-v", "file.txt"};
    ASSERT_TRUE(args.parse(v));
    EXPECT_EQ(args.get_string("token"), "");        // 未提供
    EXPECT_FALSE(args.has_flag("verbose"));
    ASSERT_EQ(args.remaining().size(), 3u);
    EXPECT_EQ(args.remaining()[0], "--token");
    EXPECT_EQ(args.remaining()[1], "-v");
    EXPECT_EQ(args.remaining()[2], "file.txt");
}

TEST(ArgsTest, UnknownOptionFails)
{
    libmini::Args args = make_args();
    const char* argv[] = {"demo", "--nope", "1"};
    EXPECT_FALSE(args.parse(3, const_cast<char**>(argv)));

    const char* argv2[] = {"demo", "-z"};
    EXPECT_FALSE(args.parse(2, const_cast<char**>(argv2)));
}

TEST(ArgsTest, MissingValueFails)
{
    libmini::Args args = make_args();
    const char* argv[] = {"demo", "--token"};
    EXPECT_FALSE(args.parse(2, const_cast<char**>(argv)));
}

TEST(ArgsTest, FlagWithValueRejected)
{
    libmini::Args args = make_args();
    const char* argv[] = {"demo", "--verbose=yes", "--token", "t"};
    EXPECT_FALSE(args.parse(4, const_cast<char**>(argv)));
}

TEST(ArgsTest, HelpRequested)
{
    libmini::Args args = make_args();
    const char* argv[] = {"demo", "--help"};
    EXPECT_FALSE(args.parse(2, const_cast<char**>(argv)));
    EXPECT_TRUE(args.help_requested());

    libmini::Args args2 = make_args();
    const char* argv2[] = {"demo", "-h"};
    EXPECT_FALSE(args2.parse(2, const_cast<char**>(argv2)));
    EXPECT_TRUE(args2.help_requested());
}

TEST(ArgsTest, ParseIsRepeatable)
{
    libmini::Args args = make_args();
    const char* first[] = {"demo", "-v", "--port", "1", "--token", "a", "in.txt"};
    ASSERT_TRUE(args.parse(7, const_cast<char**>(first)));
    EXPECT_TRUE(args.has_flag("verbose"));
    EXPECT_EQ(args.get_int("port"), 1);
    EXPECT_EQ(args.positional("input"), "in.txt");

    // 第二次解析：flag 归位、位置参数清空、默认值恢复
    const char* second[] = {"demo", "--port", "2", "--token", "b", "in2.txt"};
    ASSERT_TRUE(args.parse(6, const_cast<char**>(second)));
    EXPECT_FALSE(args.has_flag("verbose"));
    EXPECT_EQ(args.get_int("port"), 2);
    EXPECT_EQ(args.get_string("token"), "b");
    EXPECT_EQ(args.positional("input"), "in2.txt");
}

TEST(ArgsTest, VectorParseInterface)
{
    libmini::Args args = make_args();
    const std::vector<std::string> v = {"--host", "h1", "--token", "t1", "in.txt"};
    ASSERT_TRUE(args.parse(v));
    EXPECT_EQ(args.get_string("host"), "h1");
}

TEST(ArgsTest, UsageContainsOptions)
{
    libmini::Args args = make_args();
    // print_usage 输出到 stderr，这里仅验证不崩溃且可调用
    args.print_usage();
    SUCCEED();
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
