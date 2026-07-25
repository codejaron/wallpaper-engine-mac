import XCTest
@testable import Open_Wallpaper_Engine

final class SteamCmdServiceTests: XCTestCase {
    func testTimedOutLoginDirectsUserToSteamMobileApproval() {
        let result = SteamCmdService.loginResult(
            output: "",
            exitCode: -1,
            didTimeOut: true
        )

        XCTAssertEqual(result, .timedOut)
        XCTAssertEqual(
            result.errorMessage,
            "Login timed out. Check Steam Mobile for an approval request, then try again."
        )
    }
}
