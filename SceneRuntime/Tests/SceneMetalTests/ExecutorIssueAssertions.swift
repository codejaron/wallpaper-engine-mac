import Foundation
import SceneRuntimeBridge
import XCTest

extension XCTestCase {
    func assertNoExecutorIssues(
        _ executor: WESceneFrameExecutorRef,
        file: StaticString = #filePath,
        line: UInt = #line
    ) throws {
        var error: WESceneRuntimeErrorRef?
        var count = 0
        guard we_scene_frame_executor_issue_count(
            executor, &count, &error
        ) == 1 else {
            throw executorIssueFailure("count", error)
        }
        XCTAssertNil(error, file: file, line: line)
        guard count == 0 else {
            var messages: [String] = []
            for index in 0..<count {
                var issue = WESceneFrameExecutorIssueInfo()
                guard we_scene_frame_executor_issue_info(
                    executor, index, &issue, &error
                ) == 1 else {
                    throw executorIssueFailure("info", error)
                }
                messages.append(
                    issue.message.map(String.init(cString:)) ?? "<no message>"
                )
            }
            XCTFail(
                "Expected no executor issues, received: \(messages.joined(separator: " | "))",
                file: file,
                line: line
            )
            return
        }
    }

    func assertSingleSkippedObjectIssue(
        _ executor: WESceneFrameExecutorRef,
        objectIndex: Int,
        objectId: Int32,
        operationIndex: Int? = nil,
        messageContains expectedFragments: [String]
    ) throws {
        var error: WESceneRuntimeErrorRef?
        var count = 0
        guard we_scene_frame_executor_issue_count(
            executor, &count, &error
        ) == 1 else {
            throw executorIssueFailure("count", error)
        }
        XCTAssertNil(error)
        XCTAssertEqual(count, 1)

        var issue = WESceneFrameExecutorIssueInfo()
        guard we_scene_frame_executor_issue_info(
            executor, 0, &issue, &error
        ) == 1 else {
            throw executorIssueFailure("info", error)
        }
        XCTAssertNil(error)
        XCTAssertEqual(issue.severity, WE_SCENE_FRAME_ISSUE_SKIP_OBJECT)
        XCTAssertEqual(issue.object_index, objectIndex)
        XCTAssertEqual(issue.object_id, objectId)
        if let operationIndex {
            XCTAssertEqual(issue.operation_index, operationIndex)
        }
        let message = issue.message.map(String.init(cString:)) ?? ""
        for fragment in expectedFragments {
            XCTAssertTrue(
                message.localizedCaseInsensitiveContains(fragment),
                "Expected executor issue '\(message)' to contain '\(fragment)'"
            )
        }
    }

    private func executorIssueFailure(
        _ operation: String,
        _ error: WESceneRuntimeErrorRef?
    ) -> NSError {
        let message = we_scene_runtime_error_message(error)
            .map(String.init(cString:)) ?? "No error"
        we_scene_runtime_error_destroy(error)
        return NSError(
            domain: "SceneMetalTests.ExecutorIssue",
            code: 1,
            userInfo: [
                NSLocalizedDescriptionKey:
                    "Executor issue \(operation) failed: \(message)",
            ]
        )
    }
}
