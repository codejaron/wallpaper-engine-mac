import Foundation

enum WorkshopFilterGroup {
    case contentRating
    case type
    case genre
}

struct WorkshopFilterSelection: Equatable {
    var contentRatings: Set<String> = ["Everyone"]
    var types: Set<String> = []
    var genres: Set<String> = []

    func contains(_ tag: String, in group: WorkshopFilterGroup) -> Bool {
        tags(in: group).contains(tag)
    }

    mutating func set(_ tag: String, in group: WorkshopFilterGroup, isSelected: Bool) {
        switch group {
        case .contentRating:
            update(&contentRatings, tag: tag, isSelected: isSelected)
        case .type:
            update(&types, tag: tag, isSelected: isSelected)
        case .genre:
            update(&genres, tag: tag, isSelected: isSelected)
        }
    }

    var candidateTags: [String] {
        let activeGroups = [genres, types, contentRatings].filter { !$0.isEmpty }
        return activeGroups.min { $0.count < $1.count }?.sorted() ?? []
    }

    func matches(tags: [String]) -> Bool {
        let itemTags = Set(tags)
        return matches(contentRatings, itemTags: itemTags)
            && matches(types, itemTags: itemTags)
            && matches(genres, itemTags: itemTags)
    }

    private func tags(in group: WorkshopFilterGroup) -> Set<String> {
        switch group {
        case .contentRating:
            return contentRatings
        case .type:
            return types
        case .genre:
            return genres
        }
    }

    private func matches(_ selectedTags: Set<String>, itemTags: Set<String>) -> Bool {
        selectedTags.isEmpty || !selectedTags.isDisjoint(with: itemTags)
    }

    private func update(_ tags: inout Set<String>, tag: String, isSelected: Bool) {
        if isSelected {
            tags.insert(tag)
        } else {
            tags.remove(tag)
        }
    }
}
