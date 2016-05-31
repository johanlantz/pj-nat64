Pod::Spec.new do |s|
  s.name = "pj-nat64"
  s.version = "0.2.0"
  s.summary = "NAT64 workarounds for pjsip"
  s.homepage = "https://github.com/johanlantz/curly"
  s.license = { :type => "MIT", :file => "LICENSE" }
  s.author = "Johan Lantz"
  
  s.platform = :ios, "7.0"
  s.source = { :git => "https://github.com/johanlantz/pj-nat64.git", :tag => s.version.to_s }
  
  s.source_files  = "*.{h,c}"
  s.public_header_files = "pj-nat64.h"
   
end
